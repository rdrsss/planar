// @file link.t.cpp
// @brief Unit tests for `planar.engine.external.link` (plan 996, task
// 6106). Exercises the four enum text round-trips (including the
// hyphenated `sync_direction` values, which are the one place the
// enumerator name and the stored column value deliberately disagree), the
// create/show/delete surface, the UNIQUE-violation mapping, and the
// not-found distinction `delete` gets from `returning id`.
//
// The not-found case is the load-bearing one: `planar unlink 999` reports
// `error: link 999 not found` and exits 1 (oracle-captured), and that
// message exists only because this module distinguishes "deleted nothing"
// from "deleted something". A `DELETE` that reported success on zero rows
// would turn that into a silent exit 0.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.external;

namespace {

namespace link = planar::engine::external::link;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_extlink_test_{}_{}.db",
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

// `external_links.system_id` has a real foreign key onto `external_systems`
// with `on delete cascade`, and the runtime enables foreign keys per
// connection — so a link cannot be inserted without a system row. Seeded
// through raw SQL because `engine_external`'s system surface is NOT ported
// this cycle (see CMakeLists.txt); this is the one table this file has no
// engine entry point for.
auto insert_system(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into external_systems (kind, slug, default_project, auth_method, auth_ref) "
                           "values ('github-issues', ?, 'owner/repo', 'gh-cli', '') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

} // namespace

TEST_CASE("entity-kind text round-trips over all seven members", "[engine][external][link]") {
  for (auto const kind :
       {link::external_entity_kind::plan, link::external_entity_kind::task, link::external_entity_kind::question,
        link::external_entity_kind::test_scenario, link::external_entity_kind::artifact, link::external_entity_kind::decision,
        link::external_entity_kind::session}) {
    auto const text = link::external_entity_kind_to_text(kind);
    INFO("kind text: " << text);
    auto const back = link::external_entity_kind_from_text(text);
    REQUIRE(back.has_value());
    CHECK(*back == kind);
  }
  CHECK(link::external_entity_kind_to_text(link::external_entity_kind::test_scenario) == "test_scenario");
  CHECK_FALSE(link::external_entity_kind_from_text("scenario").has_value());
  CHECK_FALSE(link::external_entity_kind_from_text("").has_value());
}

TEST_CASE("link-role text round-trips", "[engine][external][link]") {
  for (auto const role : {link::link_role::mirror, link::link_role::parent, link::link_role::child, link::link_role::reference}) {
    auto const back = link::link_role_from_text(link::link_role_to_text(role));
    REQUIRE(back.has_value());
    CHECK(*back == role);
  }
  CHECK_FALSE(link::link_role_from_text("sibling").has_value());
}

TEST_CASE("sync-direction stored values are hyphenated, not the enumerator names", "[engine][external][link]") {
  // The one place the C++ enumerator and the column value deliberately
  // differ (`read_only` vs `read-only`). A port that emitted the
  // enumerator spelling would violate the CHECK constraint at INSERT time,
  // so this is pinned by literal rather than by round-trip alone.
  CHECK(link::sync_direction_to_text(link::sync_direction::read_only) == "read-only");
  CHECK(link::sync_direction_to_text(link::sync_direction::write_back) == "write-back");
  CHECK(link::sync_direction_to_text(link::sync_direction::two_way) == "two-way");
  CHECK_FALSE(link::sync_direction_from_text("read_only").has_value());
  for (auto const dir : {link::sync_direction::read_only, link::sync_direction::write_back, link::sync_direction::two_way}) {
    auto const back = link::sync_direction_from_text(link::sync_direction_to_text(dir));
    REQUIRE(back.has_value());
    CHECK(*back == dir);
  }
}

TEST_CASE("sync-status text round-trips", "[engine][external][link]") {
  for (auto const status :
       {link::sync_status::ok, link::sync_status::conflict, link::sync_status::error, link::sync_status::never}) {
    auto const back = link::sync_status_from_text(link::sync_status_to_text(status));
    REQUIRE(back.has_value());
    CHECK(*back == status);
  }
  CHECK_FALSE(link::sync_status_from_text("failed").has_value());
}

TEST_CASE("create stores every column and show reads it back", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto created = link::create(conn, link::create_args{
                                        .entity_kind    = link::external_entity_kind::task,
                                        .entity_id      = 42,
                                        .system_id      = system_id,
                                        .external_id    = "PROJ-7",
                                        .external_url   = std::string{"https://example.invalid/PROJ-7"},
                                        .role           = link::link_role::reference,
                                        .direction      = link::sync_direction::read_only,
                                        .initial_status = link::sync_status::never,
                                        .config_json    = std::string{R"({"k":1})"},
                                    });
  REQUIRE(created.has_value());
  CHECK(created->id > 0);

  auto shown = link::show(conn, created->id);
  REQUIRE(shown.has_value());
  CHECK(shown->entity_kind == link::external_entity_kind::task);
  CHECK(shown->entity_id == 42);
  CHECK(shown->system_id == system_id);
  CHECK(shown->external_id == "PROJ-7");
  REQUIRE(shown->external_url.has_value());
  CHECK(*shown->external_url == "https://example.invalid/PROJ-7");
  CHECK(shown->role == link::link_role::reference);
  CHECK(shown->direction == link::sync_direction::read_only);
  CHECK(shown->last_sync_status == link::sync_status::never);
  REQUIRE(shown->config_json.has_value());
  CHECK(*shown->config_json == R"({"k":1})");
  CHECK_FALSE(shown->last_synced_at.has_value());
  CHECK_FALSE(shown->created_at.empty());
}

TEST_CASE("unset optional columns come back as SQL NULL, not empty strings", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto created = link::create(conn, link::create_args{
                                        .entity_kind = link::external_entity_kind::plan,
                                        .entity_id   = 1,
                                        .system_id   = system_id,
                                        .external_id = "1",
                                    });
  REQUIRE(created.has_value());
  CHECK_FALSE(created->external_url.has_value());
  CHECK_FALSE(created->config_json.has_value());
  CHECK_FALSE(created->last_synced_at.has_value());
  // The engine defaults, which are NOT the `planar link` CLI defaults.
  CHECK(created->role == link::link_role::mirror);
  CHECK(created->direction == link::sync_direction::two_way);
}

TEST_CASE("show reports not_found for an id that does not exist", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            got  = link::show(conn, 999);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error() == link::link_error::not_found);
}

TEST_CASE("delete removes the row and a second delete reports not_found", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto created = link::create(conn, link::create_args{
                                        .entity_kind = link::external_entity_kind::plan,
                                        .entity_id   = 3,
                                        .system_id   = system_id,
                                        .external_id = "9",
                                    });
  REQUIRE(created.has_value());

  auto removed = link::remove(conn, created->id);
  REQUIRE(removed.has_value());

  auto gone = link::show(conn, created->id);
  REQUIRE_FALSE(gone.has_value());
  CHECK(gone.error() == link::link_error::not_found);

  // The distinction `planar unlink`'s `error: link N not found` rests on.
  auto again = link::remove(conn, created->id);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == link::link_error::not_found);
}

TEST_CASE("delete of a never-existing id reports not_found", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            got  = link::remove(conn, 999);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error() == link::link_error::not_found);
}

TEST_CASE("create maps the UNIQUE violation to link_exists", "[engine][external][link]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  link::create_args args{
      .entity_kind = link::external_entity_kind::plan,
      .entity_id   = 5,
      .system_id   = system_id,
      .external_id = "same",
  };
  REQUIRE(link::create(conn, args).has_value());

  auto again = link::create(conn, args);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == link::link_error::link_exists);
}

TEST_CASE("deleting a link detaches its sync_events rather than cascading them", "[engine][external][link]") {
  // The schema's `on delete set null`, asserted through this module because
  // it is the reason `unlink`'s long_desc promises the event history
  // survives. A cascade here would silently discard audit rows.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto created = link::create(conn, link::create_args{
                                        .entity_kind = link::external_entity_kind::plan,
                                        .entity_id   = 8,
                                        .system_id   = system_id,
                                        .external_id = "11",
                                    });
  REQUIRE(created.has_value());

  auto ins = conn.prepare("insert into sync_events (link_id, direction, outcome) values (?, 'push', 'ok')");
  REQUIRE(ins.has_value());
  REQUIRE(ins->bind_int64(1, created->id).has_value());
  REQUIRE(ins->step().has_value());

  REQUIRE(link::remove(conn, created->id).has_value());

  auto count = conn.prepare("select count(*), sum(link_id is null) from sync_events");
  REQUIRE(count.has_value());
  auto stepped = count->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  CHECK(count->column_int64(0) == 1);
  CHECK(count->column_int64(1) == 1);
}
