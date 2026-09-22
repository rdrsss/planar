// @file sync.t.cpp
// @brief Unit tests for `planar.engine.workbench.sync` (plan 996, task 6037):
// the classification table, the terminal filter, and the round trips, against
// a real migrated SQLite database and a real scratch workbench root.
//
// FILESYSTEM SAFETY. Every root below is a clock-keyed directory under
// `std::filesystem::temp_directory_path()`, removed on scope exit. This
// module takes the root as an explicit PARAMETER -- unlike the Zig original,
// which resolves it from `std.c.environ` inside `run` -- so there is no code
// path from this suite to the operator's real `~/.planar/workbench/`. That
// matters more here than anywhere: `archive` deletes a tree.
//
// ORACLE PROVENANCE. Every behavior asserted below was first observed by
// running BOTH binaries over identical pinned arenas and diffing stdout,
// stderr and exit code. The seed:
//
//   $Z init --name demo --slug demo ; $Z assoc create project:demo --kind project
//   $Z assoc add project:demo $W/proj
//   $Z plan create "Demo Feature" --slug demo-feature        -> plan 1
//   $Z task add "First Task" --plan 1 --body "Task body here." --editor=false
//   $Z task add "Second Task" --plan 1 --editor=false
//   $Z artifact add "Tech Spec: Auth" --kind tech_spec --plan 1 ...
//   $Z decision add / question add / scenario add --plan 1
//   $Z plan create "Child Milestone" --slug child-ms --parent 1
//
// (Seeding each arena INDEPENDENTLY rather than copying one is load-bearing:
// `projects.root_path` is absolute, and a copied arena silently loses
// cwd-derived scope, which changes what `task update` touches. The first
// draft copied, and the resulting "divergence" cost a round of debugging.)
//
// Key captures reproduced here:
//
//   push        7 files written; artifact at the feature ROOT, tasks under
//               tasks/cross/, child plan under plans/<slug>.md
//   pull        an FS body edit lands in the DB with the generated header
//               stripped
//   conflict    both sides moved -> one sync_events row, exit 3
//   filter      one cancelled task -> `1 filtered (mode=failures)` on push,
//               and NOTHING on pull
//   rename      changing a task's title makes its OLD manifest row read as
//               `deleted_on_fs` (`missing:` under push), NOT as a new write
//   new-on-FS   a task-kind file with no row is CREATED on pull, with a
//               derives-from link and its touches reconciled
//   double-wrap `artifact update --body @<canonical file>` renders front
//               matter inside front matter, and the file still lints clean

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.manifest;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace {

namespace ws  = planar::engine::workbench::sync;
namespace wm  = planar::engine::workbench::manifest;
namespace wfs = planar::engine::workbench::fsutil;
namespace wt  = planar::engine::workbench::terminal;

/// @brief A migrated database plus a workbench root, both scratch.
struct arena {
  std::filesystem::path                 dir_;
  std::optional<planar::db::connection> conn_;

  arena()
      : dir_(std::filesystem::temp_directory_path() / std::format("planar_wb_sync_{}_{}",
                                                                  std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                  reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(dir_ / "wb", ec);
    auto conn = planar::db::connection::open((dir_ / "planar.db").string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    conn_ = std::move(*conn);
  }
  arena(const arena&)            = delete;
  arena& operator=(const arena&) = delete;
  ~arena() {
    conn_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  auto conn() -> planar::db::connection& {
    return *conn_;
  }
  [[nodiscard]] auto root() const -> std::string {
    return (dir_ / "wb").string();
  }
};

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  REQUIRE(conn.execute(sql).has_value());
}

auto scalar_id(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto scalar_text(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief The seed arrangement above, reduced to what the engine reads.
struct seeded {
  std::int64_t assoc_id   = 0;
  std::int64_t project_id = 0;
  std::int64_t plan_id    = 0;
  std::int64_t task_one   = 0;
  std::int64_t task_two   = 0;
  std::int64_t artifact   = 0;
  std::int64_t child_plan = 0;
};

auto seed(planar::db::connection& conn) -> seeded {
  seeded out;
  exec(conn, "insert into projects (slug, name, root_path) values ('demo', 'demo', '/tmp/demo')");
  out.project_id = scalar_id(conn, "select id from projects where slug = 'demo'");
  exec(conn, "insert into associations (slug, name, kind) values ('project:demo', 'demo', 'project')");
  out.assoc_id = scalar_id(conn, "select id from associations where slug = 'project:demo'");

  exec(conn, std::format("insert into plans (scope_kind, scope_id, title, slug, summary, status) "
                         "values ('association', {}, 'Demo Feature', 'demo-feature', 'A demo.', 'draft')",
                         out.assoc_id));
  out.plan_id = scalar_id(conn, "select id from plans where slug = 'demo-feature'");

  exec(conn, std::format("insert into tasks (scope_kind, scope_id, plan_id, title, body, status, priority) "
                         "values ('association', {}, {}, 'First Task', 'Task body here.', 'todo', 100)",
                         out.assoc_id, out.plan_id));
  out.task_one = scalar_id(conn, "select id from tasks where title = 'First Task'");
  exec(conn, std::format("insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
                         "values ('association', {}, {}, 'Second Task', 'todo', 100)",
                         out.assoc_id, out.plan_id));
  out.task_two = scalar_id(conn, "select id from tasks where title = 'Second Task'");

  exec(conn, std::format("insert into artifacts (scope_kind, scope_id, kind, title, body, status) "
                         "values ('association', {}, 'tech_spec', 'Tech Spec: Auth', 'Spec body.', 'draft')",
                         out.assoc_id));
  out.artifact = scalar_id(conn, "select id from artifacts where title = 'Tech Spec: Auth'");
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('artifact', {}, 'plan', {}, 'derives-from')",
                         out.artifact, out.plan_id));

  exec(conn, std::format("insert into plans (scope_kind, scope_id, parent_plan_id, title, slug, status) "
                         "values ('association', {}, {}, 'Child Milestone', 'child-ms', 'draft')",
                         out.assoc_id, out.plan_id));
  out.child_plan = scalar_id(conn, "select id from plans where slug = 'child-ms'");
  return out;
}

auto feature_dir_of(arena& a, std::int64_t plan_id) -> std::filesystem::path {
  auto anchor = ws::fetch_anchor(a.conn(), plan_id);
  REQUIRE(anchor.has_value());
  return ws::feature_dir_for(a.root(), *anchor);
}

auto class_of(const ws::result& value, std::string_view suffix) -> ws::classification {
  for (auto const& entry : value.entries) {
    if (entry.file_path.ends_with(suffix)) {
      return entry.value;
    }
  }
  FAIL(std::format("no entry ending in {}", suffix));
  return ws::classification::no_op;
}

auto has_entry(const ws::result& value, std::string_view suffix) -> bool {
  return std::ranges::any_of(value.entries, [&](const ws::entry& e) { return e.file_path.ends_with(suffix); });
}

} // namespace

// --- layout ---------------------------------------------------------------

TEST_CASE("push writes the whole feature tree in its oracle-captured layout", "[workbench][sync][push]") {
  arena      a;
  auto const s = seed(a.conn());

  auto value = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(value.has_value());
  // Five entities: the anchor plan, the artifact linked to it, both tasks,
  // and the child plan. (The oracle's own seed had seven -- it added a
  // decision, a question and a scenario as well.)
  CHECK(value->applied == 5);
  CHECK(value->conflicts == 0);
  CHECK(value->malformed == 0);

  auto const dir = feature_dir_of(a, s.plan_id);
  CHECK(dir.string().ends_with("/project_demo/p1-demo-feature"));
  CHECK(wfs::path_exists(dir / "README.md"));
  // The artifact sits at the feature-dir ROOT, not under `artifacts/`.
  CHECK(wfs::path_exists(dir / std::format("{}-tech-spec-auth.md", s.artifact)));
  CHECK_FALSE(wfs::path_exists(dir / "artifacts"));
  CHECK(wfs::path_exists(dir / "plans" / "child-ms.md"));
  CHECK(wfs::path_exists(dir / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one)));
  CHECK(wfs::path_exists(dir / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two)));
  // The manifest mirror lands too, and every projected file has a row.
  CHECK(wfs::path_exists(dir / ".sync"));
  auto rows = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows.has_value());
  CHECK(rows->size() == value->applied);
}

TEST_CASE("the rendered README is byte-identical to the oracle's shape", "[workbench][sync][render]") {
  arena      a;
  auto const s        = seed(a.conn());
  auto       rendered = ws::render_entity(a.conn(), s.plan_id, "plan", s.plan_id);
  REQUIRE(rendered.has_value());
  CHECK(rendered->rel_path == "README.md");
  auto const created = scalar_text(a.conn(), std::format("select created_at from plans where id = {}", s.plan_id));
  auto const updated = scalar_text(a.conn(), std::format("select updated_at from plans where id = {}", s.plan_id));
  CHECK(rendered->content == std::format("---\nentity_kind: plan\nentity_id: {}\nanchor_plan_id: {}\n"
                                         "title: Demo Feature\nstatus: draft\n---\n\n"
                                         "# Plan {}: Demo Feature\n\n"
                                         "**Status:** draft  \n**Created:** {}  \n**Updated:** {}\n\nA demo.\n",
                                         s.plan_id, s.plan_id, s.plan_id, created, updated));
}

TEST_CASE("a child plan renders under plans/<slug>.md while the anchor is README.md", "[workbench][sync][render]") {
  arena      a;
  auto const s     = seed(a.conn());
  auto       child = ws::render_entity(a.conn(), s.plan_id, "plan", s.child_plan);
  REQUIRE(child.has_value());
  CHECK(child->rel_path == "plans/child-ms.md");
}

TEST_CASE("an unknown entity kind is not renderable", "[workbench][sync][render]") {
  arena      a;
  auto const s = seed(a.conn());
  CHECK_FALSE(ws::render_entity(a.conn(), s.plan_id, "session", 1).has_value());
  CHECK(ws::render_entity(a.conn(), s.plan_id, "task", 99999).error() == ws::sync_error::not_found);
}

// --- anchor resolution ----------------------------------------------------

TEST_CASE("only a TOP-LEVEL plan is an anchor", "[workbench][sync][anchor]") {
  arena      a;
  auto const s = seed(a.conn());
  CHECK(ws::fetch_anchor(a.conn(), s.plan_id).has_value());
  // A child plan id reports not_found, exactly like a nonexistent one --
  // which is why `workbench push <child>` exits 1 with `plan not found`.
  CHECK(ws::fetch_anchor(a.conn(), s.child_plan).error() == ws::sync_error::not_found);
  CHECK(ws::fetch_anchor(a.conn(), 99999).error() == ws::sync_error::not_found);
}

TEST_CASE("resolve_plan_argument distinguishes INVALID from NOT FOUND", "[workbench][sync][anchor]") {
  // The distinction is operator-visible: `workbench push 0` exits 2 with
  // `invalid plan '0'`, `workbench push 999` exits 1 with `plan not found`.
  arena      a;
  auto const s = seed(a.conn());
  CHECK(ws::resolve_plan_argument(a.conn(), std::to_string(s.plan_id))->id == s.plan_id);
  CHECK(ws::resolve_plan_argument(a.conn(), "demo-feature")->id == s.plan_id);
  CHECK(ws::resolve_plan_argument(a.conn(), "0").error() == ws::sync_error::invalid_input);
  CHECK(ws::resolve_plan_argument(a.conn(), "-1").error() == ws::sync_error::invalid_input);
  CHECK(ws::resolve_plan_argument(a.conn(), "99999").error() == ws::sync_error::not_found);
  CHECK(ws::resolve_plan_argument(a.conn(), "nosuchslug").error() == ws::sync_error::not_found);
  CHECK(ws::resolve_plan_argument(a.conn(), "").error() == ws::sync_error::invalid_input);
}

TEST_CASE("the feature-dir key is the plan's external_id when it has one", "[workbench][sync][anchor]") {
  // Hardcoding `p<id>` here once made GC compute the wrong feature directory
  // for externally-linked plans and silently skip their trees; the Zig
  // source carries that note. One definition, used by every path.
  arena      a;
  auto const s = seed(a.conn());
  CHECK(ws::fetch_anchor(a.conn(), s.plan_id)->plan_key == std::format("p{}", s.plan_id));
  exec(a.conn(), "insert into external_systems (kind, slug, auth_method, auth_ref) "
                 "values ('jira', 'jira', 'token-env', 'JIRA_TOKEN')");
  auto const system_id = scalar_id(a.conn(), "select id from external_systems where slug = 'jira'");
  exec(a.conn(), std::format("insert into external_links (entity_kind, entity_id, system_id, external_id) "
                             "values ('plan', {}, {}, 'JIRA-123')",
                             s.plan_id, system_id));
  CHECK(ws::fetch_anchor(a.conn(), s.plan_id)->plan_key == "JIRA-123");
  CHECK(feature_dir_of(a, s.plan_id).string().ends_with("/JIRA-123-demo-feature"));
}

// --- classification -------------------------------------------------------

TEST_CASE("a second push is entirely no_op", "[workbench][sync][classify]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto again = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(again.has_value());
  CHECK(again->applied == 0);
  CHECK(again->pending == 0);
  CHECK(std::ranges::all_of(again->entries, [](const ws::entry& e) { return e.value == ws::classification::no_op; }));
  // ...and it is no_op for the RIGHT reason. Without this, the case passes
  // just as happily against a push that never wrote a manifest row at all:
  // with no row the classifier falls into its no-row branch, finds the
  // file's hash equal to the rendered one, and also answers no_op. The two
  // states are indistinguishable from `entries` alone -- proven by a
  // break-probe that suppressed the upsert and left this case green. The
  // row's `db_updated_at` must match the entity, which is what makes a
  // later DB-only edit classify as `db_to_fs` rather than backwards.
  auto rows = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows.has_value());
  auto const task_row =
      std::ranges::find_if(*rows, [&](const wm::sync_state& r) { return r.entity_kind == "task" && r.entity_id == s.task_one; });
  REQUIRE(task_row != rows->end());
  CHECK(task_row->db_updated_at == scalar_text(a.conn(), std::format("select updated_at from tasks where id = {}", s.task_one)));
  CHECK(task_row->content_hash.size() == 64);
}

TEST_CASE("an FS-only edit classifies fs_to_db and pull applies it", "[workbench][sync][pull]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(
      wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: {}\nanchor_plan_id: {}\ntitle: First Task\n"
                                               "status: doing\npriority: 100\n---\n\n# Task {}: First Task\n\n"
                                               "**Status:** doing  \n\nEdited body from FS.\n",
                                               s.task_one, s.plan_id, s.task_one)));

  // `status` sees it as pending and writes nothing.
  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "1-first-task.md") == ws::classification::fs_to_db);
  CHECK(peek->pending == 1);
  CHECK(peek->applied == 0);
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "todo");

  auto applied = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(applied.has_value());
  CHECK(applied->applied == 1);
  // The generated header is stripped; only the operator's prose lands.
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == "Edited body from FS.");
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "doing");
}

TEST_CASE("a DB-only edit classifies db_to_fs and push applies it", "[workbench][sync][push]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set body = 'DB side body', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_one));

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "1-first-task.md") == ws::classification::db_to_fs);
  CHECK(peek->pending == 1);

  auto applied = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(applied.has_value());
  CHECK(applied->applied == 1);
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  CHECK(wfs::read_file(file)->find("DB side body") != std::string::npos);
}

TEST_CASE("both sides moving is a CONFLICT, recorded once and reused", "[workbench][sync][conflict]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two);
  auto       body = *wfs::read_file(file);
  body += "FS side extra\n";
  REQUIRE(wfs::write_file_atomic(file, body));
  exec(a.conn(), std::format("update tasks set body = 'DB side body', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_two));

  auto first = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(first.has_value());
  CHECK(first->conflicts == 1);
  CHECK(class_of(*first, "2-second-task.md") == ws::classification::conflict);
  auto const event_id = std::ranges::find_if(first->entries, [](const ws::entry& e) {
                          return e.value == ws::classification::conflict;
                        })->conflict_id;
  CHECK(event_id > 0);

  // Re-running must REUSE the row, or a repeated `status` would accumulate
  // one sync_events row per invocation.
  auto second = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(second.has_value());
  CHECK(second->conflicts == 1);
  CHECK(scalar_id(a.conn(), "select count(*) from sync_events where scope = 'workbench' and outcome = 'conflict'") == 1);
}

TEST_CASE("two SIMULTANEOUS conflicts dedup INDEPENDENTLY, never conflating one entity's row for another's",
          "[workbench][sync][conflict][dedup]") {
  // The single-conflict fixture above can't discriminate the dedup lookup's
  // four-clause match (anchor_plan_id, entity_kind, entity_id, file_path)
  // from a bug that reuses ANY open conflict row: with only one row to
  // find, a wrongly-permissive clause and a correct one look identical.
  // Two entities conflicting at once, each re-probed on a SECOND status()
  // call, is the minimum fixture that can tell "found the right row" from
  // "found a row".
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());

  auto const file_one = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(wfs::write_file_atomic(file_one, *wfs::read_file(file_one) + "FS side extra one\n"));
  exec(a.conn(), std::format("update tasks set body = 'DB side body one', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_one));
  auto const file_two = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two);
  REQUIRE(wfs::write_file_atomic(file_two, *wfs::read_file(file_two) + "FS side extra two\n"));
  exec(a.conn(), std::format("update tasks set body = 'DB side body two', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_two));

  auto first = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(first.has_value());
  CHECK(first->conflicts == 2);
  CHECK(class_of(*first, "1-first-task.md") == ws::classification::conflict);
  CHECK(class_of(*first, "2-second-task.md") == ws::classification::conflict);
  auto const conflict_id_of = [&](const ws::result& value, std::string_view suffix) {
    return std::ranges::find_if(value.entries, [&](const ws::entry& e) { return e.file_path.ends_with(suffix); })->conflict_id;
  };
  auto const id_one = conflict_id_of(*first, "1-first-task.md");
  auto const id_two = conflict_id_of(*first, "2-second-task.md");
  CHECK(id_one > 0);
  CHECK(id_two > 0);
  CHECK(id_one != id_two);

  // Re-running must reuse EACH entity's own row -- two total, and the SAME
  // two ids, not two new ones and not one shared one.
  auto second = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(second.has_value());
  CHECK(second->conflicts == 2);
  CHECK(conflict_id_of(*second, "1-first-task.md") == id_one);
  CHECK(conflict_id_of(*second, "2-second-task.md") == id_two);
  CHECK(scalar_id(a.conn(), "select count(*) from sync_events where scope = 'workbench' and outcome = 'conflict'") == 2);
}

TEST_CASE("two sides that CONVERGED on identical bytes are no_op, not a conflict", "[workbench][sync][conflict]") {
  // Both hashes moved off the manifest, but they moved to the same place.
  // A port that stopped at "both changed -> conflict" would report a
  // conflict that has nothing to reconcile.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set body = 'Converged.', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_two));
  auto rendered = ws::render_entity(a.conn(), s.plan_id, "task", s.task_two);
  REQUIRE(rendered.has_value());
  auto const file = feature_dir_of(a, s.plan_id) / rendered->rel_path;
  REQUIRE(wfs::write_file_atomic(file, rendered->content));

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(peek->conflicts == 0);
  CHECK(class_of(*peek, "2-second-task.md") == ws::classification::no_op);
}

TEST_CASE("a file present with NO manifest row and matching content is no_op, not db_to_fs",
          "[workbench][sync][classify][no-row]") {
  // `find_state` returns nullptr for an entity whose manifest row is gone
  // (e.g. a GC'd or hand-deleted row) but whose FS file survived. That is a
  // DIFFERENT starting point from "never pushed" (fs absent -> db_to_fs):
  // here the classifier must still notice the content already matches
  // before it re-writes anything.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  REQUIRE(wm::delete_by_entity(a.conn(), s.plan_id, "task", s.task_one).has_value());

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "1-first-task.md") == ws::classification::no_op);
  CHECK(peek->pending == 0);
}

TEST_CASE("a file present with NO manifest row and DIFFERENT content is fs_to_db, not db_to_fs",
          "[workbench][sync][classify][no-row]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  REQUIRE(wm::delete_by_entity(a.conn(), s.plan_id, "task", s.task_one).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(
      wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: {}\nanchor_plan_id: {}\ntitle: First Task\n"
                                               "status: todo\npriority: 100\n---\n\n# Task {}: First Task\n\n"
                                               "**Status:** todo  \n\nEdited with no manifest row.\n",
                                               s.task_one, s.plan_id, s.task_one)));

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "1-first-task.md") == ws::classification::fs_to_db);
  CHECK(peek->pending == 1);

  auto applied = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(applied.has_value());
  CHECK(applied->applied == 1);
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == "Edited with no manifest row.");
}

TEST_CASE("sync_both applies BOTH directions in one call, unlike push or pull alone", "[workbench][sync][bidirectional]") {
  // Every classification-apply gate in `run` reads
  // `run_mode == mode::push || run_mode == mode::sync` (or the pull
  // equivalent) -- but until now no fixture ever called `sync_both`, so a
  // mode::sync-specific defect (a wrong enum comparison, a branch that
  // silently only half-applies) had no test standing between it and a
  // green suite. Two DIFFERENT entities move on DIFFERENT sides so a
  // single-field fixture could not tell "per-entity direction routing"
  // from "the whole call happened to go one way".
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());

  // task_one moves on the FS side only.
  auto const file_one = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(wfs::write_file_atomic(file_one,
                                 std::format("---\nentity_kind: task\nentity_id: {}\nanchor_plan_id: {}\ntitle: First Task\n"
                                             "status: doing\npriority: 100\n---\n\n# Task {}: First Task\n\n"
                                             "**Status:** doing  \n\nFS side edit for sync_both.\n",
                                             s.task_one, s.plan_id, s.task_one)));
  // task_two moves on the DB side only.
  exec(a.conn(), std::format("update tasks set body = 'DB side edit for sync_both', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_two));

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "1-first-task.md") == ws::classification::fs_to_db);
  CHECK(class_of(*peek, "2-second-task.md") == ws::classification::db_to_fs);
  CHECK(peek->pending == 2);
  CHECK(peek->applied == 0);

  auto synced = ws::sync_both(a.conn(), s.plan_id, a.root());
  REQUIRE(synced.has_value());
  CHECK(synced->applied == 2);
  CHECK(synced->pending == 0);
  CHECK(synced->conflicts == 0);
  // The FS edit landed in the DB...
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == "FS side edit for sync_both.");
  // ...and the DB edit landed on disk, in the SAME call.
  auto const file_two = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two);
  CHECK(wfs::read_file(file_two)->find("DB side edit for sync_both") != std::string::npos);

  auto again = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(again.has_value());
  CHECK(again->pending == 0);
  CHECK(std::ranges::all_of(again->entries, [](const ws::entry& e) { return e.value == ws::classification::no_op; }));
}

TEST_CASE("sync_both soft-deletes an FS-deleted entity, unlike push alone", "[workbench][sync][bidirectional][delete]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(wfs::remove_file(file));

  auto synced = ws::sync_both(a.conn(), s.plan_id, a.root());
  REQUIRE(synced.has_value());
  CHECK(synced->applied == 1);
  CHECK(synced->pending == 0);
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "cancelled");
}

TEST_CASE("resolving a conflict keeps the chosen side and settles the event", "[workbench][sync][resolve]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two);
  REQUIRE(wfs::write_file_atomic(file, *wfs::read_file(file) + "FS side extra\n"));
  exec(a.conn(), std::format("update tasks set body = 'DB side body', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_two));
  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  auto const event_id = std::ranges::find_if(peek->entries, [](const ws::entry& e) {
                          return e.value == ws::classification::conflict;
                        })->conflict_id;

  REQUIRE(ws::resolve_conflict(a.conn(), a.root(), event_id, ws::conflict_resolution::db).has_value());
  CHECK(wfs::read_file(file)->find("FS side extra") == std::string::npos);
  CHECK(scalar_text(a.conn(), std::format("select outcome from sync_events where id = {}", event_id)) == "resolved-db");

  // A SECOND resolve of the same event is INVALID INPUT, not not-found --
  // the oracle exits 2 for it and 1 for an unknown id.
  CHECK(ws::resolve_conflict(a.conn(), a.root(), event_id, ws::conflict_resolution::fs).error() == ws::sync_error::invalid_input);
  CHECK(ws::resolve_conflict(a.conn(), a.root(), 99999, ws::conflict_resolution::fs).error() == ws::sync_error::not_found);
}

TEST_CASE("a deleted file soft-deletes its entity on pull, and only pending on push", "[workbench][sync][delete]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(wfs::remove_file(file));

  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(pushed.has_value());
  CHECK(class_of(*pushed, "1-first-task.md") == ws::classification::deleted_on_fs);
  // Push does NOT apply a deletion -- and it does not re-write the file
  // either, because the class is `deleted_on_fs` rather than `db_to_fs`.
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "todo");

  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "cancelled");
}

TEST_CASE("an UNLINKED entity's orphaned manifest row is cleaned only on pull/sync, not push",
          "[workbench][sync][delete][orphan]") {
  // Distinct from the tracked-entity delete above: this entity is no longer
  // ENUMERATED at all (its derives-from link is gone), so it never enters
  // the main entity loop and its manifest row is only reachable through the
  // "manifest rows the DB no longer enumerates" sweep at the tail of `run`.
  // That sweep has its OWN pull/sync mode gate, separate from the
  // classification switch's.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / std::format("{}-tech-spec-auth.md", s.artifact);
  REQUIRE(wfs::path_exists(file));
  exec(a.conn(), std::format("delete from entity_links where from_kind = 'artifact' and from_id = {} "
                             "and to_kind = 'plan' and to_id = {} and relationship = 'derives-from'",
                             s.artifact, s.plan_id));
  REQUIRE(wfs::remove_file(file));

  auto rows_before = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows_before.has_value());
  auto const had_row_before = std::ranges::any_of(
      *rows_before, [&](const wm::sync_state& r) { return r.entity_kind == "artifact" && r.entity_id == s.artifact; });
  REQUIRE(had_row_before);

  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(pushed.has_value());
  CHECK(pushed->pending >= 1);
  CHECK(scalar_text(a.conn(), std::format("select status from artifacts where id = {}", s.artifact)) == "draft");
  auto rows_after_push = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows_after_push.has_value());
  CHECK(std::ranges::any_of(*rows_after_push,
                            [&](const wm::sync_state& r) { return r.entity_kind == "artifact" && r.entity_id == s.artifact; }));

  auto synced = ws::sync_both(a.conn(), s.plan_id, a.root());
  REQUIRE(synced.has_value());
  CHECK(scalar_text(a.conn(), std::format("select status from artifacts where id = {}", s.artifact)) == "retired");
  auto rows_after_sync = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows_after_sync.has_value());
  CHECK_FALSE(std::ranges::any_of(
      *rows_after_sync, [&](const wm::sync_state& r) { return r.entity_kind == "artifact" && r.entity_id == s.artifact; }));
}

TEST_CASE("a RENAMED entity reads as deleted_on_fs, not as a fresh write", "[workbench][sync][rename]") {
  // Non-obvious and oracle-verified: changing a task's title changes its
  // filename, so the manifest row points at a path that no longer matches
  // the rendered one. The result is `missing:` under push, and the OLD file
  // is left on disk.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set title = 'Renamed First Task', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}",
                             s.task_one));

  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(pushed.has_value());
  CHECK(class_of(*pushed, "1-renamed-first-task.md") == ws::classification::deleted_on_fs);
  CHECK(pushed->applied == 0);
  CHECK(wfs::path_exists(feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one)));
}

// --- malformed ------------------------------------------------------------

TEST_CASE("a malformed file is counted and never applied, in EVERY mode", "[workbench][sync][malformed]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const junk = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / "junk.md";
  REQUIRE(wfs::write_file_atomic(junk, "no front matter at all\n"));

  for (auto const& value : {ws::status(a.conn(), s.plan_id, a.root()), ws::pull(a.conn(), s.plan_id, a.root()),
                            ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false)}) {
    REQUIRE(value.has_value());
    CHECK(value->malformed == 1);
    REQUIRE(value->malformed_files.size() == 1);
    CHECK(value->malformed_files[0].path.ends_with("junk.md"));
    CHECK(value->malformed_files[0].parse_error == "MalformedFrontmatter");
    // No entity was created from it, in any mode.
    CHECK(scalar_id(a.conn(), "select count(*) from tasks") == 2);
  }
  // And the file is still there -- sync never deletes what it cannot read.
  CHECK(wfs::path_exists(junk));
}

TEST_CASE("a malformed file for a TRACKED entity classifies malformed, not fs_to_db", "[workbench][sync][malformed]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(wfs::write_file_atomic(file, "broken\n"));

  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  CHECK(class_of(*pulled, "1-first-task.md") == ws::classification::malformed);
  // Crucially: the broken content did NOT reach the database.
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == "Task body here.");
}

// --- new on FS ------------------------------------------------------------

TEST_CASE("a new task-kind file is created on pull, linked and touch-reconciled", "[workbench][sync][new]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / "new-one.md";
  REQUIRE(wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: 99\n"
                                                   "anchor_plan_id: {}\ntitle: Brand New Task\nstatus: todo\n"
                                                   "touches:\n- demo\n---\n\nSome new body.\n",
                                                   s.plan_id)));

  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(class_of(*peek, "new-one.md") == ws::classification::new_on_fs);
  CHECK(scalar_id(a.conn(), "select count(*) from tasks") == 2);

  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  auto const new_id = scalar_id(a.conn(), "select id from tasks where title = 'Brand New Task'");
  // The front matter's entity_id (99) is IGNORED -- the row gets a fresh id.
  CHECK(new_id != 99);
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", new_id)) == "todo");
  CHECK(scalar_id(a.conn(), std::format("select priority from tasks where id = {}", new_id)) == 100);
  CHECK(scalar_id(a.conn(), std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                        "and to_kind = 'plan' and relationship = 'derives-from'",
                                        new_id)) == 1);
  CHECK(scalar_id(a.conn(), std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                        "and to_kind = 'repo' and relationship = 'touches'",
                                        new_id)) == 1);
}

TEST_CASE("a new NON-task file is reported and left alone", "[workbench][sync][new]") {
  // The workbench auto-creates tasks and nothing else.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "decisions" / "new-decision.md";
  REQUIRE(wfs::write_file_atomic(file, std::format("---\nentity_kind: decision\nentity_id: 42\n"
                                                   "anchor_plan_id: {}\ntitle: A Decision\nstatus: proposed\n---\n",
                                                   s.plan_id)));
  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  CHECK(class_of(*pulled, "new-decision.md") == ws::classification::new_on_fs);
  CHECK(scalar_id(a.conn(), "select count(*) from decisions") == 0);
  CHECK(wfs::path_exists(file));
}

TEST_CASE("a touches slug naming no known project is silently skipped", "[workbench][sync][new]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / "unknown-touch.md";
  REQUIRE(wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: {}\n"
                                                   "title: Unknown Touch\nstatus: todo\ntouches:\n- nosuchrepo\n---\n",
                                                   s.plan_id)));
  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  auto const new_id = scalar_id(a.conn(), "select id from tasks where title = 'Unknown Touch'");
  // The task IS created; the unresolvable slug simply produces no edge, and
  // is not an error and not a project creation.
  CHECK(new_id > 0);
  CHECK(scalar_id(a.conn(), std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                        "and to_kind = 'repo'",
                                        new_id)) == 0);
}

// --- the terminal filter --------------------------------------------------

TEST_CASE("push filters a terminal entity; pull does NOT", "[workbench][sync][filter]") {
  arena      a;
  auto const s = seed(a.conn());
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", s.task_two));

  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(pushed.has_value());
  CHECK(pushed->filtered == 1);
  CHECK_FALSE(has_entry(*pushed, "2-second-task.md"));
  CHECK_FALSE(wfs::path_exists(feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two)));

  // Pull enumerates it normally: a terminal entity's file is still a
  // legitimate INPUT.
  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  CHECK(pulled->filtered == 0);
  CHECK(has_entry(*pulled, "2-second-task.md"));
}

TEST_CASE("the ANCHOR PLAN is exempt from the filter", "[workbench][sync][filter]") {
  // Dropping it would break feature-tree navigation -- there would be no
  // README.md at the root of an abandoned feature.
  arena      a;
  auto const s = seed(a.conn());
  exec(a.conn(), std::format("update plans set status = 'abandoned' where id = {}", s.plan_id));
  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::all, false);
  REQUIRE(pushed.has_value());
  CHECK(wfs::path_exists(feature_dir_of(a, s.plan_id) / "README.md"));
  CHECK(has_entry(*pushed, "README.md"));
}

TEST_CASE("filter-mode all additionally drops a SUCCESS terminal", "[workbench][sync][filter]") {
  arena      a;
  auto const s = seed(a.conn());
  exec(a.conn(), std::format("update tasks set status = 'done' where id = {}", s.task_two));

  auto failures = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(failures.has_value());
  CHECK(failures->filtered == 0);
  CHECK(failures->filter_mode == "failures");

  arena      b;
  auto const t = seed(b.conn());
  exec(b.conn(), std::format("update tasks set status = 'done' where id = {}", t.task_two));
  auto all = ws::push(b.conn(), t.plan_id, b.root(), wt::mode::all, false);
  REQUIRE(all.has_value());
  CHECK(all->filtered == 1);
  CHECK(all->filter_mode == "all");
}

TEST_CASE("a pre-existing terminal file is counted, and cleaned only on request", "[workbench][sync][filter][cleanup]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two);
  REQUIRE(wfs::path_exists(file));
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", s.task_two));

  auto reported = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(reported.has_value());
  CHECK(reported->filtered == 1);
  CHECK(reported->pre_existing_terminal == 1);
  CHECK(reported->cleaned == 0);
  CHECK(wfs::path_exists(file)); // reported, NOT removed

  auto cleaned = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, true);
  REQUIRE(cleaned.has_value());
  CHECK(cleaned->pre_existing_terminal == 1);
  CHECK(cleaned->cleaned == 1);
  CHECK_FALSE(wfs::path_exists(file));
  // The manifest row goes too, or the next push would read the missing file
  // as drift.
  auto rows = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows.has_value());
  CHECK(
      std::ranges::none_of(*rows, [&](const wm::sync_state& r) { return r.entity_kind == "task" && r.entity_id == s.task_two; }));
}

// --- archive / restore ----------------------------------------------------

TEST_CASE("archive removes the tree and the manifest rows", "[workbench][sync][archive]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const dir = feature_dir_of(a, s.plan_id);
  REQUIRE(wfs::path_exists(dir));

  auto archived = ws::archive(a.conn(), s.plan_id, a.root());
  REQUIRE(archived.has_value());
  CHECK(*archived == dir.string());
  CHECK_FALSE(wfs::path_exists(dir));
  CHECK(wm::load(a.conn(), s.plan_id)->empty());
  // No entity row was touched -- archive is filesystem-only.
  CHECK(scalar_text(a.conn(), std::format("select status from tasks where id = {}", s.task_one)) == "todo");
}

TEST_CASE("archiving an ALREADY-ABSENT tree succeeds and still reports the path", "[workbench][sync][archive]") {
  // Oracle-probed: `workbench archive 1` twice in a row exits 0 both times.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::archive(a.conn(), s.plan_id, a.root()).has_value());
  auto again = ws::archive(a.conn(), s.plan_id, a.root());
  REQUIRE(again.has_value());
  CHECK(*again == feature_dir_of(a, s.plan_id).string());
}

TEST_CASE("restore re-materializes the tree and honours the same filter as push", "[workbench][sync][restore]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  REQUIRE(ws::archive(a.conn(), s.plan_id, a.root()).has_value());
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", s.task_two));

  auto restored = ws::restore(a.conn(), s.plan_id, a.root(), wt::mode::failures);
  REQUIRE(restored.has_value());
  auto const dir = feature_dir_of(a, s.plan_id);
  CHECK(wfs::path_exists(dir / "README.md"));
  CHECK(wfs::path_exists(dir / ".sync"));
  CHECK(wfs::path_exists(dir / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one)));
  // The cancelled task is skipped, so the restored tree mirrors what a
  // fresh push would write rather than what was there before.
  CHECK_FALSE(wfs::path_exists(dir / "tasks" / "cross" / std::format("{}-second-task.md", s.task_two)));
}

// --- list -----------------------------------------------------------------

TEST_CASE("list_active reports every top-level plan and whether it has a tree", "[workbench][sync][list]") {
  arena      a;
  auto const s      = seed(a.conn());
  auto       before = ws::list_active(a.conn(), a.root());
  REQUIRE(before.has_value());
  REQUIRE(before->size() == 1); // the CHILD plan is not listed
  CHECK((*before)[0].plan_id == s.plan_id);
  CHECK((*before)[0].assoc_slug == "project:demo");
  CHECK((*before)[0].plan_key == std::format("p{}", s.plan_id));
  CHECK_FALSE((*before)[0].has_fs_tree);

  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto after = ws::list_active(a.conn(), a.root());
  REQUIRE(after.has_value());
  CHECK((*after)[0].has_fs_tree);
}

TEST_CASE("a GLOBAL-scope plan lists its assoc as `global` but is pathed with an EMPTY one", "[workbench][sync][list]") {
  // The two queries genuinely disagree: `list_active` coalesces a missing
  // association to the literal `global` (display text) while `fetch_anchor`
  // coalesces it to the empty string (which collapses the directory level).
  // The consequence is that `has_fs_tree` looks under `<root>/global/...`
  // while `push` writes to `<root>/p<id>-<slug>`, so a global-scope plan
  // reports `no-tree` even after a successful push. Reproduced from the Zig
  // originals, which have the same split, and pinned here so it is a known
  // quirk rather than a surprise.
  arena a;
  exec(a.conn(), "insert into plans (scope_kind, title, slug, status) values ('global', 'G', 'global-plan', 'draft')");
  auto const plan_id = scalar_id(a.conn(), "select id from plans where slug = 'global-plan'");
  REQUIRE(ws::push(a.conn(), plan_id, a.root(), wt::mode::failures, false).has_value());

  auto const dir = feature_dir_of(a, plan_id);
  CHECK(dir.string().ends_with(std::format("/wb/p{}-global-plan", plan_id)));
  CHECK(wfs::path_exists(dir / "README.md"));

  auto items = ws::list_active(a.conn(), a.root());
  REQUIRE(items.has_value());
  REQUIRE(items->size() == 1);
  CHECK((*items)[0].assoc_slug == "global");
  CHECK_FALSE((*items)[0].has_fs_tree);
}

// --- body extraction ------------------------------------------------------

TEST_CASE("extract_body_text strips the generated header and nothing else", "[workbench][sync][body]") {
  CHECK(ws::extract_body_text("# Task 1: T\n\n**Status:** todo  \n**Priority:** 100  \n\nReal body.\n") == "Real body.");
  // Once prose has started, later header-shaped lines are KEPT -- the strip
  // is a prefix operation, not a filter.
  CHECK(ws::extract_body_text("# H\n\nprose\n\n**Status:** x\n") == "prose\n\n**Status:** x");
  CHECK(ws::extract_body_text("").empty());
  CHECK(ws::extract_body_text("# Only a heading\n\n**Only:** labels\n").empty());
  CHECK(ws::extract_body_text("   \n\n  body  \n\n") == "body");
  // A line containing ":**" that does NOT start with "**" is real prose,
  // not a generated label, and must NOT be skipped -- isolates the
  // `starts_with("**")` half of `is_label` from its `find(":**")` half.
  CHECK(ws::extract_body_text("See **Notes:** here\n") == "See **Notes:** here");
  // A line that DOES start with "**" but has no ":**" anywhere is real
  // bold prose, not a generated label -- isolates the `find(":**")` half.
  CHECK(ws::extract_body_text("**bold** prose\n") == "**bold** prose");
}

TEST_CASE("extract_body_text does NOT strip a `---` line", "[workbench][sync][body][double-wrap]") {
  // This is what makes the double-wrap sticky: an artifact body that already
  // carries front matter keeps it on the way back in, so the nested block
  // survives every subsequent round trip.
  CHECK(ws::extract_body_text("# Artifact 1: T\n\n**Kind:** tech_spec  \n\n## Content\n\n---\nentity_kind: artifact\n"
                              "---\n\ninner\n")
            .starts_with("## Content"));
  CHECK(ws::extract_body_text("---\nentity_kind: artifact\n---\n").starts_with("---"));
}

TEST_CASE("the double-wrap survives a full push/pull/push round trip", "[workbench][sync][double-wrap]") {
  // End-to-end reproduction of the live hazard: feeding an artifact its own
  // canonical workbench file as a body renders front matter INSIDE front
  // matter, and every subsequent cycle preserves it exactly. Verified
  // byte-for-byte against the oracle.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const file      = feature_dir_of(a, s.plan_id) / std::format("{}-tech-spec-auth.md", s.artifact);
  auto const canonical = *wfs::read_file(file);

  auto stmt = a.conn().prepare(std::format(
      "update artifacts set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+1 second') where id = {}", s.artifact));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, canonical).has_value());
  REQUIRE(stmt->step().has_value());

  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const wrapped = *wfs::read_file(file);
  // Two front-matter openings, one nested inside the other's body.
  CHECK(wrapped.starts_with("---\nentity_kind: artifact\n"));
  CHECK(wrapped.find("## Content\n\n---\nentity_kind: artifact\n") != std::string::npos);
  // And it still PARSES -- which is exactly why the defect goes unnoticed.
  auto stable = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(stable.has_value());
  CHECK(stable->malformed == 0);
  CHECK(*wfs::read_file(file) == wrapped);
}

// --- task 6422: the `pull_to_db` failure clause ----------------------------
//
// `(pull||sync) && fs_content && pull_to_db(...)` had no test reaching
// `pull_to_db` returning FALSE, so the whole success clause could be made
// permissive and the suite stayed green (task 6416's sweep).
//
// It could not be reached through the public surface, and that was TRACED
// rather than assumed: for task/plan, `parse::parse`'s `statuses_for_kind`
// rejects any status that would trip the table's CHECK constraint before
// `pull_to_db` ever runs; for artifact/decision/question/scenario the pull
// writes only `body`, which carries no CHECK constraint. There is no
// operator-reachable input that makes the write fail.
//
// The guard is therefore a DEFENSIVE one against future drift between
// `parse.cpp`'s status lists and the schema's constraints -- drift that is
// likely over time, because migrations and parser status lists are edited by
// different changes. So it is closed by fault injection rather than recorded
// as untestable (task 6422 offered both dispositions and preferred this one).
//
// The injection is a temporary BEFORE UPDATE trigger rather than the table
// rename task 6189 used: a rename would also break the classification pass's
// READS and the case would pass for the wrong reason. A trigger fails exactly
// the write under test and nothing else -- which is also what the real drift
// would look like.
TEST_CASE("a pull whose DB write fails is reported pending, not applied", "[workbench][sync][pull][6422]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());

  auto const file = feature_dir_of(a, s.plan_id) / std::format("{}-tech-spec-auth.md", s.artifact);
  REQUIRE(wfs::path_exists(file));
  // APPEND to what push rendered rather than hand-writing front matter: a
  // hand-written header classifies as `malformed`, which is a different arm
  // and would make this case vacuous.
  REQUIRE(wfs::write_file_atomic(file, *wfs::read_file(file) + "Edited body from FS.\n"));

  // Confirm the fixture really is the fs_to_db arm before injecting, so a
  // classification change cannot turn this into a vacuous pass.
  auto peek = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(peek.has_value());
  CHECK(peek->pending == 1);

  auto const before = scalar_text(a.conn(), std::format("select body from artifacts where id = {}", s.artifact));

  exec(a.conn(), "create trigger reject_artifact_body_write before update of body on artifacts "
                 "begin select raise(abort, 'simulated schema drift'); end");

  auto applied = ws::pull(a.conn(), s.plan_id, a.root());
  // The run SUCCEEDS: a failed entity write is a soft skip, not a failure of
  // the whole sync. That is the contract the clause encodes.
  REQUIRE(applied.has_value());
  CHECK(applied->applied == 0);
  CHECK(applied->pending == 1);
  // And nothing landed. The savepoint inside `pull_to_db` rolled the write
  // back, so a later run still sees the FS edit as outstanding rather than
  // finding a half-applied row.
  CHECK(scalar_text(a.conn(), std::format("select body from artifacts where id = {}", s.artifact)) == before);

  exec(a.conn(), "drop trigger reject_artifact_body_write");

  // Non-vacuity: with the injected failure removed, the SAME pull applies.
  // Without this the case would pass against a pull that never works at all.
  auto recovered = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(recovered.has_value());
  CHECK(recovered->applied == 1);
  CHECK(scalar_text(a.conn(), std::format("select body from artifacts where id = {}", s.artifact))
            .contains("Edited body from FS."));
}

// --- task 6780: pull_to_db's savepoint on the TASK arm's two writes -------
//
// Task 6422 closed the sibling clause (the pull_to_db success clause itself)
// but explicitly could not reach the savepoint's OWN rollback: its fixture
// used `artifact`, which performs exactly one write, so there is nothing
// partial to undo. The savepoint only earns its keep on `kind == "task"`,
// which performs TWO writes -- the body/status update, then
// reconcile_touches -- and this is the case where the first can succeed
// while the second fails.
TEST_CASE("pulling a task whose touches reconcile fails rolls back its body update too", "[workbench][sync][pull][6780]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());

  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  // A `touches:` list that DIFFERS from the DB (which has none), so
  // `reconcile_touches` actually performs an insert -- and a body edit, so
  // the first write has something to roll back if the second fails.
  REQUIRE(wfs::write_file_atomic(
      file, std::format("---\nentity_kind: task\nentity_id: {}\nanchor_plan_id: {}\ntitle: First Task\n"
                        "status: doing\ntouches:\n- demo\n---\n\n**Status:** doing  \n\nEdited body from FS.\n",
                        s.task_one, s.plan_id)));

  auto const before = scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one));

  // Reject exactly the entity_links write reconcile_touches performs, leaving
  // the tasks update free to succeed on its own.
  exec(a.conn(), "create trigger reject_touch_link before insert on entity_links "
                 "when new.relationship = 'touches' "
                 "begin select raise(abort, 'simulated touches-reconcile failure'); end");

  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  exec(a.conn(), "drop trigger reject_touch_link");

  // The run still SUCCEEDS -- a failed entity write is a soft skip, matching
  // task 6422's contract for the sibling clause.
  REQUIRE(pulled.has_value());
  CHECK(pulled->applied == 0);
  CHECK(pulled->pending == 1);

  // The savepoint's whole job: the body update, which succeeded on its own,
  // did NOT survive the later failure. Without the rollback this is false --
  // the task ends up with an edited body but no touches edge, a
  // half-applied pull.
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == before);
  CHECK(scalar_id(a.conn(), std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                        "and to_kind = 'repo' and relationship = 'touches'",
                                        s.task_one)) == 0);

  // Non-vacuity: with the trigger removed, the identical pull applies both
  // writes.
  auto recovered = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(recovered.has_value());
  CHECK(recovered->applied == 1);
  CHECK(scalar_text(a.conn(), std::format("select body from tasks where id = {}", s.task_one)) == "Edited body from FS.");
  CHECK(scalar_id(a.conn(), std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                        "and to_kind = 'repo' and relationship = 'touches'",
                                        s.task_one)) == 1);
}

TEST_CASE("a SUCCESSFUL pull leaves the connection in autocommit, savepoint released", "[workbench][sync][pull][6787]") {
  // The sibling defect to 6780's rollback clause, and deliberately probed
  // SEPARATELY (6780's acceptance criteria said so outright): deleting
  // `pull_to_db`'s SUCCESS-path `release savepoint` survived the whole
  // suite, because a leaked savepoint fails silently. Nothing wraps the
  // per-file loop in a transaction, so the leak carries into the NEXT
  // file's `pull_to_db`, which re-enters a same-named NESTED savepoint --
  // legal in SQLite, no error -- and the connection is simply left
  // non-autocommit, with every later write hanging off a scope nobody will
  // commit.
  //
  // That state was unobservable from a test until `connection::
  // in_transaction()` (added for this task), which is why the mutation
  // survived. Asserting it directly is the discriminating strategy chosen
  // here, over contriving a multi-file scenario where the leak changes
  // another file's outcome.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());

  // Precondition: nothing is open before the pull, so a `true` afterwards
  // can only have come from the pull itself.
  REQUIRE_FALSE(a.conn().in_transaction());

  auto const file = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / std::format("{}-first-task.md", s.task_one);
  REQUIRE(
      wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: {}\nanchor_plan_id: {}\ntitle: First Task\n"
                                               "status: doing\n---\n\n**Status:** doing  \n\nEdited body from FS.\n",
                                               s.task_one, s.plan_id)));

  auto pulled = ws::pull(a.conn(), s.plan_id, a.root());
  REQUIRE(pulled.has_value());
  CHECK(pulled->applied == 1);

  // The assertion this case exists for.
  CHECK_FALSE(a.conn().in_transaction());
}

TEST_CASE("a scenario's own status is read, so a RETIRED one filters", "[workbench][sync][filter][scenario]") {
  // The seed above carries no scenario at all -- its comment notes the
  // oracle's seed had one and this one does not -- so `fetch_status`'s
  // `test_scenario` arm was never reached by any fixture. Closes a
  // break-probe SURVIVOR (task 6781): dropping that arm makes the status
  // lookup return an EMPTY string, which classifies as active, so a
  // retired scenario is projected forever and `workbench gc` never
  // reclaims its file.
  //
  // Which of the clause's two spellings is live was settled by probing,
  // not by reading the schema: `entity_links.from_kind` stores
  // `test_scenario` (migration 00004 admits no `scenario`), but
  // `append_derived_entities` CANONICALISES it in SQL --
  // `case when from_kind = 'test_scenario' then 'scenario' else from_kind
  // end` -- so `fetch_status` only ever sees `scenario`. The
  // `test_scenario` spelling in the same clause is therefore unreachable
  // on this path and is recorded as dead rather than given its own
  // fixture; dropping it leaves every test green.
  auto const with_status = [](planar::db::connection& conn, const seeded& s, std::string_view status) {
    exec(conn, std::format("insert into test_scenarios (scope_kind, scope_id, title, body, status) "
                           "values ('association', {}, 'Login Scenario', 'Given a user.', '{}')",
                           s.assoc_id, status));
    auto const id = scalar_id(conn, "select id from test_scenarios where title = 'Login Scenario'");
    exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('test_scenario', {}, 'plan', {}, 'derives-from')",
                           id, s.plan_id));
  };

  // `retired` is the scenario table's failure terminal, so it filters.
  arena      a;
  auto const s = seed(a.conn());
  with_status(a.conn(), s, "retired");
  auto retired = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(retired.has_value());
  CHECK(retired->filtered == 1);

  // `ready` is active, and the SAME push filters nothing -- which is what
  // proves the status was actually read rather than the scenario being
  // dropped for its kind.
  arena      b;
  auto const t = seed(b.conn());
  with_status(b.conn(), t, "ready");
  auto ready = ws::push(b.conn(), t.plan_id, b.root(), wt::mode::failures, false);
  REQUIRE(ready.has_value());
  CHECK(ready->filtered == 0);
}

TEST_CASE("status is READ-ONLY and creates no feature directory", "[workbench][sync][status]") {
  // `run` skips `make_path_all` for `mode::status` alone. Closes a
  // break-probe SURVIVOR (task 6781): dropping that mode check let a plain
  // `workbench status` MATERIALISE the feature directory on disk, and no
  // fixture noticed -- every other status case runs after a push has
  // already created it, so the directory existed either way.
  arena      a;
  auto const s   = seed(a.conn());
  auto const dir = feature_dir_of(a, s.plan_id);
  REQUIRE_FALSE(wfs::path_exists(dir));

  auto value = ws::status(a.conn(), s.plan_id, a.root());
  REQUIRE(value.has_value());
  CHECK_FALSE(wfs::path_exists(dir));
}

TEST_CASE("a workbench root with a TRAILING SLASH stores the same paths", "[workbench][sync][path]") {
  // `to_stored_path` strips the separator between the root and the rest
  // only when one is actually there: with a trailing-slash root the
  // remainder already begins at the first real character. Closes a
  // break-probe SURVIVOR (task 6781): dropping the `front() == '/'` half
  // made the strip unconditional, which eats the first character of every
  // stored path (`tasks/...` -> `asks/...`). Every existing fixture passes
  // a root WITHOUT a trailing slash, so nothing noticed.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root() + "/", wt::mode::failures, false).has_value());

  // The strip is only REACHED for a file on disk that no manifest row
  // claims, so a push alone cannot see it -- the second pass is what makes
  // `to_stored_path` recompute a name and compare it against the rows the
  // push wrote. A mutant that strips unconditionally yields
  // `roject_demo/...`, which matches nothing, so every already-synced file
  // reappears as UNCLAIMED.
  auto second = ws::status(a.conn(), s.plan_id, a.root() + "/");
  REQUIRE(second.has_value());
  auto const unclaimed =
      std::ranges::count_if(second->entries, [](const ws::entry& e) { return e.value == ws::classification::new_on_fs; });
  INFO("unclaimed entries: " << unclaimed);
  CHECK(unclaimed == 0);
}

// --- task 6880: a value whose byte length lands on the 256-byte boundary ----
//
// `planar workbench status/push/pull` aborted with SIGABRT (`__stack_chk_fail`)
// rendering scenario 3025 of plan 1065. The row's body is 255 CHARACTERS but
// 256 BYTES (it contains one `§`, two bytes in UTF-8), and every scratch
// repro that counted characters missed the boundary. Reproduced standalone
// against the pinned libc++ (23.1.1): `std::format_to(std::back_inserter(s),
// "\n\n{}\n", arg)` with `arg.size() % 256 == 0` writes one byte past the
// formatter's 256-byte stack buffer -- upstream llvm/llvm-project#154670.
// The fixtures below seed the exact shapes that reach the boundary through
// the public renderers; before the fix each one aborts the test process.

namespace {

auto insert_scenario_with_body(planar::db::connection& conn, const seeded& s, std::string_view body) -> std::int64_t {
  auto stmt = conn.prepare(std::format("insert into test_scenarios (scope_kind, scope_id, title, body, status) "
                                       "values ('association', {}, 'Boundary Scenario', ?, 'draft')",
                                       s.assoc_id));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, body).has_value());
  REQUIRE(stmt->step().has_value());
  auto const id = scalar_id(conn, "select max(id) from test_scenarios");
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('test_scenario', {}, 'plan', {}, 'derives-from')",
                         id, s.plan_id));
  return id;
}

} // namespace

TEST_CASE("a scenario body of exactly 256 BYTES (255 chars) renders and pushes", "[workbench][sync][render][6880]") {
  arena      a;
  auto const s = seed(a.conn());
  // 254 ASCII bytes + one two-byte `§` = 255 characters, 256 bytes: the live
  // row's shape, and the one a character count cannot see.
  std::string const live_shape = std::string(254, 'x') + "\xc2\xa7";
  REQUIRE(live_shape.size() == 256);
  auto const id = insert_scenario_with_body(a.conn(), s, live_shape);

  auto rendered = ws::render_entity(a.conn(), s.plan_id, "scenario", id);
  REQUIRE(rendered.has_value());
  CHECK(rendered->content.ends_with("\n\n" + live_shape + "\n"));

  // The whole-tree walk, which is what the CLI verbs run.
  auto pushed = ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false);
  REQUIRE(pushed.has_value());
  CHECK(has_entry(*pushed, std::format("scenarios/{}-boundary-scenario.md", id)));
}

TEST_CASE("every multiple of 256 bytes is a boundary, not only the first", "[workbench][sync][render][6880]") {
  arena      a;
  auto const s = seed(a.conn());
  for (auto const n : {std::size_t{256}, std::size_t{512}, std::size_t{1024}}) {
    std::string const body(n, 'y');
    auto const        id       = insert_scenario_with_body(a.conn(), s, body);
    auto              rendered = ws::render_entity(a.conn(), s.plan_id, "scenario", id);
    INFO("body bytes: " << n);
    REQUIRE(rendered.has_value());
    CHECK(rendered->content.ends_with("\n\n" + body + "\n"));
  }
}

TEST_CASE("a question body or answer of 256 bytes renders", "[workbench][sync][render][6880]") {
  arena      a;
  auto const s = seed(a.conn());
  std::string const boundary(256, 'q');
  auto const insert = [&](std::string_view title, std::string_view body, std::string_view answer) {
    auto stmt = a.conn().prepare(std::format("insert into questions (scope_kind, scope_id, title, body, answer_body, "
                                             "answered_at, status) values ('association', {}, ?, ?, ?, "
                                             "case when ? = '' then null else '2026-01-01T00:00:00.000Z' end, 'open')",
                                             s.assoc_id));
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, title).has_value());
    REQUIRE(stmt->bind_text(2, body).has_value());
    REQUIRE(stmt->bind_text(3, answer).has_value());
    REQUIRE(stmt->bind_text(4, answer).has_value());
    REQUIRE(stmt->step().has_value());
    auto const id = scalar_id(a.conn(), "select max(id) from questions");
    exec(a.conn(), std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                               "values ('question', {}, 'plan', {}, 'derives-from')",
                               id, s.plan_id));
    return id;
  };

  auto const body_id = insert("Body On Boundary", boundary, "");
  auto       body_rendered = ws::render_entity(a.conn(), s.plan_id, "question", body_id);
  REQUIRE(body_rendered.has_value());
  CHECK(body_rendered->content.ends_with("\n" + boundary + "\n"));

  auto const answer_id       = insert("Answer On Boundary", "short body", boundary);
  auto       answer_rendered = ws::render_entity(a.conn(), s.plan_id, "question", answer_id);
  REQUIRE(answer_rendered.has_value());
  CHECK(answer_rendered->content.find("\n**Answer:** " + boundary + "\n") != std::string::npos);
}

TEST_CASE("a task body of 256 bytes followed by a next action renders", "[workbench][sync][render][6880]") {
  // This one does NOT abort before the fix: the task renderer writes no
  // literal after either argument (`"\n{}"`, `"\n**Next action:** {}"`), and
  // the hazard needs a literal to land one past the full buffer. It pins the
  // task arm of the same class so the shape cannot drift into the hazard.
  arena      a;
  auto const s = seed(a.conn());
  std::string const boundary(256, 't');
  auto stmt = a.conn().prepare(std::format("update tasks set body = ?, next_action = 'do the thing', "
                                           "due_at = '2026-01-02T00:00:00.000Z' where id = {}",
                                           s.task_one));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, boundary).has_value());
  REQUIRE(stmt->step().has_value());

  auto rendered = ws::render_entity(a.conn(), s.plan_id, "task", s.task_one);
  REQUIRE(rendered.has_value());
  CHECK(rendered->content.find("\n" + boundary + "\n**Next action:** do the thing\n") != std::string::npos);
}
