// @file init.t.cpp
// @brief Unit tests for `planar.engine.config.init` (plan 996, task 6032).
// Exercises `register_cwd`'s idempotent insert-or-ignore semantics, the
// `force` checkout-moved repoint path, and `derive_slug`'s slugification
// rules, mirroring zig/src/engine/init.zig's own test suite.
//
// Include-before-import is deliberate (see db/db.t.cpp / engine/identity/
// scope.t.cpp): MSVC's supported direction for mixing textual std headers
// with IFC imports is include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.config.init;

using planar::engine::config::derive_slug;
using planar::engine::config::init_error;
using planar::engine::config::init_result;
using planar::engine::config::register_cwd;
using planar::engine::config::register_cwd_args;
using planar::engine::config::render_init_json;
using planar::engine::config::render_init_text;

namespace {

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Mirrors
/// db.t.cpp / migrate.t.cpp / engine/identity/scope.t.cpp's identical
/// helper.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_config_init_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

} // namespace

TEST_CASE("derive_slug: lowercases and collapses non-alphanumeric runs", "[init]") {
  CHECK(derive_slug("My.Project!") == "my-project");
}

TEST_CASE("derive_slug: trims leading and trailing dashes", "[init]") {
  CHECK(derive_slug("  hello  ") == "hello");
}

TEST_CASE("derive_slug: empty input falls back to \"project\"", "[init]") {
  CHECK(derive_slug("") == "project");
}

TEST_CASE("derive_slug: only-punctuation input falls back to \"project\"", "[init]") {
  CHECK(derive_slug("!!!") == "project");
}

TEST_CASE("register_cwd: happy path inserts a project row", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto result = register_cwd(conn, register_cwd_args{.cwd = "/home/user/myrepo", .git_remote = "git@github.com:user/myrepo.git"});
  REQUIRE(result.has_value());
  CHECK(result->slug == "myrepo");
  CHECK(result->name == "myrepo");
  CHECK(result->root_path == "/home/user/myrepo");
  CHECK(result->git_remote == "git@github.com:user/myrepo.git");
}

TEST_CASE("register_cwd: a second call from the same cwd is idempotent", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first = register_cwd(conn, register_cwd_args{.cwd = "/work/idemp"});
  REQUIRE(first.has_value());
  auto second = register_cwd(conn, register_cwd_args{.cwd = "/work/idemp"});
  REQUIRE(second.has_value());
  CHECK(first->id == second->id);

  auto count_stmt = conn.prepare("select count(*) from projects");
  REQUIRE(count_stmt.has_value());
  REQUIRE(count_stmt->step().has_value());
  CHECK(count_stmt->column_int64(0) == 1);
}

TEST_CASE("register_cwd: without force, a second checkout at a new path leaves the existing row untouched", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first = register_cwd(conn, register_cwd_args{.cwd = "/work/old/mover"});
  REQUIRE(first.has_value());
  auto second = register_cwd(conn, register_cwd_args{.cwd = "/work/new/mover"}); // same basename -> same derived slug
  REQUIRE(second.has_value());
  CHECK(first->id == second->id);
  CHECK(second->root_path == "/work/old/mover");
}

TEST_CASE("register_cwd: force repoints root_path and git_remote under the same id", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first = register_cwd(conn, register_cwd_args{.cwd = "/work/old/mover"});
  REQUIRE(first.has_value());

  auto forced =
      register_cwd(conn, register_cwd_args{.cwd = "/work/new/mover", .git_remote = "git@example.com:mover.git", .force = true});
  REQUIRE(forced.has_value());
  CHECK(forced->id == first->id);
  CHECK(forced->root_path == "/work/new/mover");
  CHECK(forced->git_remote == "git@example.com:mover.git");

  auto count_stmt = conn.prepare("select count(*) from projects");
  REQUIRE(count_stmt.has_value());
  REQUIRE(count_stmt->step().has_value());
  CHECK(count_stmt->column_int64(0) == 1);
}

TEST_CASE("register_cwd: rejects a non-absolute path", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto result = register_cwd(conn, register_cwd_args{.cwd = "relative/path"});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == init_error::invalid_path);
}

TEST_CASE("register_cwd: rejects an empty path", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto result = register_cwd(conn, register_cwd_args{.cwd = ""});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == init_error::invalid_path);
}

TEST_CASE("register_cwd: explicit name and slug overrides win", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto result = register_cwd(conn, register_cwd_args{.cwd = "/work/something", .name = "Friendly Name", .slug = "custom-slug"});
  REQUIRE(result.has_value());
  CHECK(result->slug == "custom-slug");
  CHECK(result->name == "Friendly Name");
}

TEST_CASE("register_cwd: git_remote is nullable", "[init]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto result = register_cwd(conn, register_cwd_args{.cwd = "/work/noremote"});
  REQUIRE(result.has_value());
  CHECK_FALSE(result->git_remote.has_value());
}

// ---------------------------------------------------------------------------
// The `planar init` renderers (plan 996, task 6132).
//
// Both return COMPLETE payloads — the oracle's exact bytes, trailing newline
// included — so the handler writes them verbatim and appends nothing. Every
// expectation below is bytes the Zig reference binary wrote under a pinned
// scratch environment, read back through `cat -e`.
//
// The rules these cases exist to pin, in the order they are easiest to get
// wrong:
//
//   1. The five project keys are OMITTED, never emitted as `null`.
//   2. `git_remote` is omitted INDEPENDENTLY of the other four — a git
//      repository with no `origin` yields four project keys and no fifth.
//   3. The `next:` hint pair is nested inside the project-slug check AND
//      guarded again on `root_path`, so a row with a NULL root path prints
//      the `project:` line and no hints.
//   4. Strings are JSON-escaped. The oracle hand-rolled this once and
//      produced invalid JSON for a project name containing a quote; that is
//      why it now routes every field through `std.json.Stringify.value`.

TEST_CASE("render_init_text: the full payload, project and hints included", "[init][render]") {
  init_result const result{
      .db             = "/tmp/arena/planar.db",
      .schema_version = 33,
      .project_id     = 1,
      .project_slug   = "proj",
      .project_name   = "proj",
      .root_path      = "/tmp/arena/proj",
  };
  CHECK(render_init_text(result) == "planar initialized\n"
                                    "  db:      /tmp/arena/planar.db\n"
                                    "  schema:  33\n"
                                    "  project: proj (id: 1)\n"
                                    "  next:    `planar assoc create project:proj --kind project`\n"
                                    "           `planar assoc add project:proj /tmp/arena/proj`\n");
}

TEST_CASE("render_init_text: --skip-project stops after the schema line", "[init][render]") {
  init_result const result{.db = "/tmp/arena/planar.db", .schema_version = 33};
  CHECK(render_init_text(result) == "planar initialized\n"
                                    "  db:      /tmp/arena/planar.db\n"
                                    "  schema:  33\n");
}

TEST_CASE("render_init_text: a project with no root path prints no hints", "[init][render]") {
  // The second guard, exercised on its own. Flattening the two checks into
  // one would print an `assoc add` line ending in a space.
  init_result const result{
      .db = "/tmp/arena/planar.db", .schema_version = 33, .project_id = 4, .project_slug = "proj", .project_name = "proj"};
  CHECK(render_init_text(result) == "planar initialized\n"
                                    "  db:      /tmp/arena/planar.db\n"
                                    "  schema:  33\n"
                                    "  project: proj (id: 4)\n");
}

TEST_CASE("render_init_json: every key present, in the oracle's order", "[init][render]") {
  init_result const result{
      .db             = "/tmp/arena/planar.db",
      .schema_version = 33,
      .project_id     = 1,
      .project_slug   = "proj",
      .project_name   = "proj",
      .root_path      = "/tmp/arena/proj",
      .git_remote     = "git@github.com:example/repo.git",
  };
  CHECK(render_init_json(result) == R"({"ok":true,"db":"/tmp/arena/planar.db","schema_version":33,"project_id":1,)"
                                    R"("project_slug":"proj","project_name":"proj","root_path":"/tmp/arena/proj",)"
                                    R"("git_remote":"git@github.com:example/repo.git"})"
                                    "\n");
}

TEST_CASE("render_init_json: an absent git_remote is OMITTED, not null", "[init][render]") {
  init_result const result{
      .db             = "/tmp/arena/planar.db",
      .schema_version = 33,
      .project_id     = 1,
      .project_slug   = "proj",
      .project_name   = "proj",
      .root_path      = "/tmp/arena/proj",
  };
  auto const rendered = render_init_json(result);
  CHECK(rendered == R"({"ok":true,"db":"/tmp/arena/planar.db","schema_version":33,"project_id":1,)"
                    R"("project_slug":"proj","project_name":"proj","root_path":"/tmp/arena/proj"})"
                    "\n");
  CHECK_FALSE(rendered.contains("git_remote"));
  CHECK_FALSE(rendered.contains("null"));
}

TEST_CASE("render_init_json: --skip-project emits ok, db and schema_version only", "[init][render]") {
  init_result const result{.db = "/tmp/arena/planar.db", .schema_version = 33};
  CHECK(render_init_json(result) == R"({"ok":true,"db":"/tmp/arena/planar.db","schema_version":33})"
                                    "\n");
}

TEST_CASE("render_init_json: strings are escaped", "[init][render]") {
  // The defect the oracle's own hand-rolled template had: a quote in a
  // project name produced a document no parser would accept.
  init_result const result{
      .db             = R"(/tmp/a"b\c)",
      .schema_version = 33,
      .project_id     = 7,
      .project_slug   = "proj",
      .project_name   = R"(He said "hi")",
      .root_path      = "/tmp/proj\n",
  };
  auto const rendered = render_init_json(result);
  CHECK(rendered.contains(R"("db":"/tmp/a\"b\\c")"));
  CHECK(rendered.contains(R"("project_name":"He said \"hi\"")"));
  CHECK(rendered.contains(R"("root_path":"/tmp/proj\n")"));
  // Exactly one real newline: the terminator.
  CHECK(std::ranges::count(rendered, '\n') == 1);
}
