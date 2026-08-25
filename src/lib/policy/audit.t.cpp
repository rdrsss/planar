// @file audit.t.cpp
// @brief Unit tests for `planar.policy.audit` (plan 1001, task 6100).
//
// ORACLE PROVENANCE. Every rule asserted here was derived by RUNNING the
// Zig binary against scratch databases and reading `audit_log` back, not
// from reading the Zig source. The full captured table is transcribed in
// audit.cppm's file header; the three findings the source would not have
// given up, and which these cases pin, are:
//
//   1. `actor` and `scope` are ALWAYS NULL on the CLI path.
//   2. `summary` is genuinely NULL for `update` and `delete`, and for the
//      `status_change` that `task update --status` writes.
//   3. A refused mutation writes NO row.
//
// The unset-optional cases matter more than they look: `bind_text` maps an
// empty string_view to the empty STRING, so a naive implementation stores
// `''` where the oracle stores NULL. Nothing observable moves, and the
// `ix_audit_log_scope` partial index (`where scope is not null`) silently
// starts including the row.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.policy.audit;

namespace {

namespace au = planar::policy::audit;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_audit_test_{}_{}.db",
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

auto scalar_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
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

} // namespace

TEST_CASE("record writes one row carrying every field it was given", "[policy][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto ok = au::record(conn, au::record_args{.verb    = au::verb::create,
                                             .entity  = {.kind = "task", .id = 42},
                                             .actor   = "session:99",
                                             .scope   = "acme",
                                             .summary = "create task 'T'"});
  REQUIRE(ok.has_value());

  CHECK(scalar_int(conn, "select count(*) from audit_log") == 1);
  CHECK(scalar_int(conn, "select count(*) from audit_log where verb = 'create' and entity_kind = 'task' "
                         "and entity_id = 42 and actor = 'session:99' and scope = 'acme' "
                         "and summary = 'create task ''T'''") == 1);
  // The column exists and defaults; the port must not be writing it itself.
  CHECK(scalar_int(conn, "select count(*) from audit_log where recorded_at is not null and recorded_at != ''") == 1);
}

// This is the arm that separates SQL NULL from the empty string. The
// difference is invisible in every rendered surface, and `bind_text` maps
// an empty string_view to `''`, so an unset optional MUST route through
// `bind_null` -- which is what this asserts, via `typeof`, not via a
// comparison that `''` would also satisfy.
TEST_CASE("unset actor, scope and summary are SQL NULL, not the empty string", "[policy][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto ok = au::record(conn, au::record_args{.verb = au::verb::delete_, .entity = {.kind = "annotation", .id = 7}});
  REQUIRE(ok.has_value());

  CHECK(scalar_text(conn, "select typeof(actor) from audit_log where id = 1") == "null");
  CHECK(scalar_text(conn, "select typeof(scope) from audit_log where id = 1") == "null");
  CHECK(scalar_text(conn, "select typeof(summary) from audit_log where id = 1") == "null");
  CHECK(scalar_int(conn, "select count(*) from audit_log where actor is null and scope is null and summary is null") == 1);
}

TEST_CASE("every verb spelling satisfies the schema's CHECK constraint", "[policy][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // A spelling the CHECK rejects fails the INSERT, so this doubles as the
  // proof that `verb_to_text` agrees with migration 14 for all six.
  for (const auto v :
       {au::verb::create, au::verb::update, au::verb::delete_, au::verb::status_change, au::verb::link, au::verb::unlink}) {
    auto ok = au::record(conn, au::record_args{.verb = v, .entity = {.kind = "task", .id = 1}});
    INFO("verb = " << au::verb_to_text(v));
    REQUIRE(ok.has_value());
  }
  CHECK(scalar_int(conn, "select count(*) from audit_log") == 6);
  CHECK(scalar_int(conn, "select count(distinct verb) from audit_log") == 6);
  // `delete_`'s trailing underscore is a C++ keyword dodge and must NOT
  // reach the column.
  CHECK(scalar_int(conn, "select count(*) from audit_log where verb = 'delete'") == 1);
  CHECK(scalar_int(conn, "select count(*) from audit_log where verb = 'delete_'") == 0);
}

TEST_CASE("record surfaces write_failed rather than reporting a phantom row", "[policy][audit]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string()).value();
  // No migrations: `audit_log` does not exist, so `prepare` fails.
  auto ok = au::record(conn, au::record_args{.verb = au::verb::create, .entity = {.kind = "task", .id = 1}});
  REQUIRE_FALSE(ok.has_value());
  CHECK(ok.error() == au::audit_error::write_failed);
}

TEST_CASE("record does not sanitise the strings it is handed", "[policy][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Bound positionally, so a quote in a summary is data. The oracle's own
  // summaries embed single quotes (`create annotation 'A1'`), which is
  // exactly the shape that breaks under interpolation.
  auto ok = au::record(conn, au::record_args{.verb    = au::verb::create,
                                             .entity  = {.kind = "annotation", .id = 1},
                                             .summary = "create annotation 'it''s'; drop table audit_log; --"});
  REQUIRE(ok.has_value());
  CHECK(scalar_int(conn, "select count(*) from audit_log") == 1);
  CHECK(scalar_text(conn, "select summary from audit_log where id = 1") == "create annotation 'it''s'; drop table audit_log; --");
}
