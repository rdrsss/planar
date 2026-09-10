// @file gc.t.cpp
// @brief Unit tests for `planar.engine.workbench.gc` (plan 996, task 6037):
// the keep rules and the drift refusal.
//
// FILESYSTEM SAFETY. `gc` UNLINKS FILES. Every root below is a clock-keyed
// scratch directory removed on scope exit, and the module takes the root as
// an explicit parameter -- it cannot reach the operator's real workbench.
//
// ORACLE PROVENANCE, from a real scratch tree with one cancelled task:
//
//   $Z workbench gc 1 --dry-run
//     workbench gc (--dry-run): would remove 1, keep 6, drifted-skipped 0, errors 0 (mode=failures)
//   $Z workbench gc 1
//     workbench gc: removed 1, kept 6, drifted-skipped 0, errors 0 (mode=failures)
//   $Z workbench gc 1                 [after appending a line to the file]
//     exit 1, and NOTHING on either stream -- see task 6122; the oracle
//     loses its refusal message to an unflushed buffer.
//   $Z workbench gc 1 --yes
//     workbench gc: removed 1, kept 7, ...
//   $Z workbench gc --all-scopes --json
//     {"removed":0,"kept":6,"drifted_skipped":0,"errors":0,"dry_run":false,"filter_mode":"failures"}
//
// All five diffed clean against the C++ binary in identical pinned arenas.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.gc;
import planar.engine.workbench.manifest;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace {

namespace wg  = planar::engine::workbench::gc;
namespace ws  = planar::engine::workbench::sync;
namespace wm  = planar::engine::workbench::manifest;
namespace wfs = planar::engine::workbench::fsutil;
namespace wt  = planar::engine::workbench::terminal;

struct arena {
  std::filesystem::path                 dir_;
  std::optional<planar::db::connection> conn_;

  arena()
      : dir_(std::filesystem::temp_directory_path() / std::format("planar_wb_gc_{}_{}",
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

struct seeded {
  std::int64_t plan_id  = 0;
  std::int64_t task_one = 0;
  std::int64_t task_two = 0;
};

auto seed(planar::db::connection& conn) -> seeded {
  seeded out;
  exec(conn, "insert into associations (slug, name, kind) values ('project:demo', 'demo', 'project')");
  auto const assoc_id = scalar_id(conn, "select id from associations where slug = 'project:demo'");
  exec(conn, std::format("insert into plans (scope_kind, scope_id, title, slug, status) "
                         "values ('association', {}, 'Demo', 'demo-feature', 'draft')",
                         assoc_id));
  out.plan_id = scalar_id(conn, "select id from plans where slug = 'demo-feature'");
  exec(conn, std::format("insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
                         "values ('association', {}, {}, 'First Task', 'todo', 100)",
                         assoc_id, out.plan_id));
  out.task_one = scalar_id(conn, "select id from tasks where title = 'First Task'");
  exec(conn, std::format("insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
                         "values ('association', {}, {}, 'Second Task', 'todo', 100)",
                         assoc_id, out.plan_id));
  out.task_two = scalar_id(conn, "select id from tasks where title = 'Second Task'");
  return out;
}

auto feature_dir_of(arena& a, std::int64_t plan_id) -> std::filesystem::path {
  auto anchor = ws::fetch_anchor(a.conn(), plan_id);
  REQUIRE(anchor.has_value());
  return ws::feature_dir_for(a.root(), *anchor);
}

/// @brief Push, then mark `task_id` terminal so gc has something to collect.
auto push_then_cancel(arena& a, const seeded& s, std::int64_t task_id) -> std::filesystem::path {
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto rendered = ws::render_entity(a.conn(), s.plan_id, "task", task_id);
  REQUIRE(rendered.has_value());
  auto const file = feature_dir_of(a, s.plan_id) / rendered->rel_path;
  REQUIRE(wfs::path_exists(file));
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", task_id));
  return file;
}

} // namespace

TEST_CASE("gc removes a terminal-backed file and drops its manifest row", "[workbench][gc]") {
  arena      a;
  auto const s    = seed(a.conn());
  auto const file = push_then_cancel(a, s, s.task_two);

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->removed == 1);
  CHECK(value->kept == 2); // README.md and the still-todo task
  CHECK(value->errors == 0);
  CHECK_FALSE(wfs::path_exists(file));
  auto rows = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows.has_value());
  CHECK(std::ranges::none_of(*rows, [&](const wm::sync_state& r) { return r.entity_id == s.task_two; }));
}

TEST_CASE("gc mutates NO entity row", "[workbench][gc]") {
  // The verb is filesystem-first. Unlike `pull`, it cannot cancel a task --
  // it only removes the projection of one that is already terminal.
  arena      a;
  auto const s = seed(a.conn());
  push_then_cancel(a, s, s.task_two);
  REQUIRE(wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{}).has_value());
  CHECK(scalar_id(a.conn(), "select count(*) from tasks") == 2);
  CHECK(scalar_id(a.conn(), std::format("select count(*) from tasks where id = {} and status = 'todo'", s.task_one)) == 1);
}

TEST_CASE("dry-run counts what it would remove and touches nothing", "[workbench][gc]") {
  arena      a;
  auto const s    = seed(a.conn());
  auto const file = push_then_cancel(a, s, s.task_two);

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.dry_run = true});
  REQUIRE(value.has_value());
  CHECK(value->removed == 1);
  CHECK(wfs::path_exists(file));
  auto rows = wm::load(a.conn(), s.plan_id);
  REQUIRE(rows.has_value());
  CHECK(std::ranges::any_of(*rows, [&](const wm::sync_state& r) { return r.entity_id == s.task_two; }));
}

TEST_CASE("a non-terminal file is KEPT", "[workbench][gc]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->removed == 0);
  CHECK(value->kept == 3);
}

TEST_CASE("filter-mode all additionally collects a SUCCESS terminal", "[workbench][gc]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set status = 'done' where id = {}", s.task_two));

  auto failures = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.dry_run = true});
  REQUIRE(failures.has_value());
  CHECK(failures->removed == 0);

  auto all = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.dry_run = true, .filter_mode = wt::mode::all});
  REQUIRE(all.has_value());
  CHECK(all->removed == 1);
}

TEST_CASE("an UNPARSEABLE file is kept, never deleted", "[workbench][gc][safety]") {
  // "I do not understand this file" must never mean "delete it".
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const junk = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / "junk.md";
  REQUIRE(wfs::write_file_atomic(junk, "no front matter\n"));

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.filter_mode = wt::mode::all});
  REQUIRE(value.has_value());
  CHECK(value->removed == 0);
  CHECK(wfs::path_exists(junk));
}

TEST_CASE("a file whose backing entity is GONE is kept", "[workbench][gc][safety]") {
  // `fetch_status` returns an empty string for a missing row, which is not a
  // recognized status, so `is_filtered_str` yields unset and the caller
  // keeps. An orphaned file is not automatically garbage.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto const orphan = feature_dir_of(a, s.plan_id) / "tasks" / "cross" / "orphan.md";
  REQUIRE(wfs::write_file_atomic(orphan, std::format("---\nentity_kind: task\nentity_id: 99999\n"
                                                     "anchor_plan_id: {}\ntitle: Orphan\nstatus: cancelled\n---\n",
                                                     s.plan_id)));
  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->removed == 0);
  CHECK(wfs::path_exists(orphan));
}

TEST_CASE("gc reads the FILE's front matter, not the manifest's mapping", "[workbench][gc]") {
  // A hand-edited file claiming a different entity_id is classified by what
  // it CLAIMS. Found while probing the oracle: a file written with
  // `entity_id: 99` was kept even though its manifest row pointed at a
  // cancelled task, because gc looked up entity 99 and found nothing.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  auto rendered = ws::render_entity(a.conn(), s.plan_id, "task", s.task_two);
  REQUIRE(rendered.has_value());
  auto const file = feature_dir_of(a, s.plan_id) / rendered->rel_path;
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", s.task_two));
  REQUIRE(wfs::write_file_atomic(file, std::format("---\nentity_kind: task\nentity_id: 99999\n"
                                                   "anchor_plan_id: {}\ntitle: Second Task\nstatus: cancelled\n---\n",
                                                   s.plan_id)));

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.yes = true});
  REQUIRE(value.has_value());
  CHECK(value->removed == 0);
  CHECK(wfs::path_exists(file));
}

// --- the drift refusal ----------------------------------------------------

TEST_CASE("a DRIFTED terminal file is held back, not removed", "[workbench][gc][drift]") {
  arena      a;
  auto const s    = seed(a.conn());
  auto const file = push_then_cancel(a, s, s.task_two);
  REQUIRE(wfs::write_file_atomic(file, *wfs::read_file(file) + "unpulled edit\n"));

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->drifted_skipped == 1);
  CHECK(value->removed == 0);
  REQUIRE(value->drifted_paths.size() == 1);
  CHECK(value->drifted_paths[0] == file.string());
  CHECK(wfs::path_exists(file));
}

TEST_CASE("yes discards the drift and removes anyway", "[workbench][gc][drift]") {
  arena      a;
  auto const s    = seed(a.conn());
  auto const file = push_then_cancel(a, s, s.task_two);
  REQUIRE(wfs::write_file_atomic(file, *wfs::read_file(file) + "unpulled edit\n"));

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{.yes = true});
  REQUIRE(value.has_value());
  CHECK(value->drifted_skipped == 0);
  CHECK(value->removed == 1);
  CHECK_FALSE(wfs::path_exists(file));
}

TEST_CASE("a file with NO manifest row is not drifted", "[workbench][gc][drift]") {
  // There is nothing to have drifted FROM, so an untracked terminal file is
  // removable without `--yes`. The alternative reading -- "unknown means
  // drifted" -- would make gc refuse on a tree it had never synced.
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", s.task_two));
  auto rendered = ws::render_entity(a.conn(), s.plan_id, "task", s.task_two);
  REQUIRE(rendered.has_value());
  auto const file = feature_dir_of(a, s.plan_id) / rendered->rel_path;
  REQUIRE(wm::delete_by_entity(a.conn(), s.plan_id, "task", s.task_two).has_value());

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->drifted_skipped == 0);
  CHECK(value->removed == 1);
  CHECK_FALSE(wfs::path_exists(file));
}

TEST_CASE("drift blocks ONLY the drifted file; the rest of the sweep proceeds", "[workbench][gc][drift]") {
  arena      a;
  auto const s = seed(a.conn());
  REQUIRE(ws::push(a.conn(), s.plan_id, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id in ({}, {})", s.task_one, s.task_two));
  auto drifted_render = ws::render_entity(a.conn(), s.plan_id, "task", s.task_two);
  REQUIRE(drifted_render.has_value());
  auto const drifted = feature_dir_of(a, s.plan_id) / drifted_render->rel_path;
  REQUIRE(wfs::write_file_atomic(drifted, *wfs::read_file(drifted) + "edit\n"));

  auto value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->drifted_skipped == 1);
  CHECK(value->removed == 1); // the clean one still went
  CHECK(wfs::path_exists(drifted));
}

// --- no tree, and --all-scopes -------------------------------------------

TEST_CASE("a plan with no tree on disk is a clean no-op", "[workbench][gc]") {
  arena      a;
  auto const s     = seed(a.conn());
  auto       value = wg::run_for_plan(a.conn(), s.plan_id, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->removed == 0);
  CHECK(value->kept == 0);
  CHECK(value->errors == 0);
}

TEST_CASE("a non-anchor or unknown plan id is silently ignored", "[workbench][gc]") {
  arena      a;
  auto const s     = seed(a.conn());
  auto       value = wg::run_for_plan(a.conn(), 99999, a.root(), wg::options{});
  REQUIRE(value.has_value());
  CHECK(value->errors == 0);
  static_cast<void>(s);
}

TEST_CASE("all-scopes aggregates across every top-level plan", "[workbench][gc]") {
  arena      a;
  auto const s = seed(a.conn());
  push_then_cancel(a, s, s.task_two);
  // A second, independent anchor with its own cancelled task.
  exec(a.conn(), "insert into plans (scope_kind, title, slug, status) values ('global', 'Other', 'other', 'draft')");
  auto const other = scalar_id(a.conn(), "select id from plans where slug = 'other'");
  exec(a.conn(), std::format("insert into tasks (scope_kind, plan_id, title, status, priority) "
                             "values ('global', {}, 'Other Task', 'todo', 100)",
                             other));
  auto const other_task = scalar_id(a.conn(), "select id from tasks where title = 'Other Task'");
  REQUIRE(ws::push(a.conn(), other, a.root(), wt::mode::failures, false).has_value());
  exec(a.conn(), std::format("update tasks set status = 'cancelled' where id = {}", other_task));

  auto value = wg::run_all_scopes(a.conn(), a.root(), wg::options{.all_scopes = true});
  REQUIRE(value.has_value());
  CHECK(value->removed == 2);
  CHECK(value->errors == 0);
}
