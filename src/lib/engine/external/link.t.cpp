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

// ---------------------------------------------------------------------------
// `load_existing_mirror` / `record_mirror_link` — task 6335.
//
// Both came out of `zig/src/engine/extsync/` and landed HERE rather than in
// `planar.engine.extsync`, because that bucket carries no `db` edge and
// these are nothing but SQL against the two tables this module owns. See
// link.cppm and the extsync bucket's CMakeLists.
// ---------------------------------------------------------------------------

TEST_CASE("load_existing_mirror answers the empty string for no row and the id for one", "[engine][external][link][mirror]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");

  // THE ABSENT CASE, and on its own it can pass for the wrong reason: an
  // implementation that always returned the empty string would satisfy it.
  // The PRESENT case below is what rules that out.
  auto const absent = link::load_existing_mirror(conn, "task", 5, system_id);
  REQUIRE(absent.has_value());
  CHECK(absent->empty());

  auto created = link::create(conn, link::create_args{
                                        .entity_kind = link::external_entity_kind::task,
                                        .entity_id   = 5,
                                        .system_id   = system_id,
                                        .external_id = "owner/repo#7",
                                        .role        = link::link_role::mirror,
                                    });
  REQUIRE(created.has_value());

  auto const present = link::load_existing_mirror(conn, "task", 5, system_id);
  REQUIRE(present.has_value());
  CHECK(*present == "owner/repo#7");

  // Same entity, DIFFERENT system: absent. Without this the query could
  // ignore `system_id` and both cases above would still pass.
  auto const other_system = insert_system(conn, "gh2");
  auto const elsewhere    = link::load_existing_mirror(conn, "task", 5, other_system);
  REQUIRE(elsewhere.has_value());
  CHECK(elsewhere->empty());

  // Same id, different KIND: absent. `plan:5` and `task:5` are different
  // entities and a query that dropped `entity_kind` would conflate them.
  auto const other_kind = link::load_existing_mirror(conn, "plan", 5, system_id);
  REQUIRE(other_kind.has_value());
  CHECK(other_kind->empty());
}

TEST_CASE("load_existing_mirror ignores a non-mirror link on the same entity", "[engine][external][link][mirror]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");

  // A `reference` row — what `planar link` writes — must NOT suppress
  // propagation. `ext propagate-one` reads this as "not yet propagated".
  auto created = link::create(conn, link::create_args{
                                        .entity_kind = link::external_entity_kind::task,
                                        .entity_id   = 9,
                                        .system_id   = system_id,
                                        .external_id = "owner/repo#3",
                                        .role        = link::link_role::reference,
                                    });
  REQUIRE(created.has_value());

  auto const found = link::load_existing_mirror(conn, "task", 9, system_id);
  REQUIRE(found.has_value());
  CHECK(found->empty());
}

TEST_CASE("record_mirror_link writes the link AND its push/ok sync event", "[engine][external][link][mirror]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");

  auto const link_id = link::record_mirror_link(conn, "plan", 4, system_id, "DEMO-1", "https://example.invalid/DEMO-1",
                                                link::sync_direction::two_way);
  REQUIRE(link_id.has_value());

  auto row = conn.prepare("select entity_kind, entity_id, external_id, external_url, link_role, sync_direction, "
                          "last_sync_status from external_links where id = ?");
  REQUIRE(row.has_value());
  REQUIRE(row->bind_int64(1, *link_id).has_value());
  auto stepped = row->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  CHECK(row->column_text(0) == "plan");
  CHECK(row->column_int64(1) == 4);
  CHECK(row->column_text(2) == "DEMO-1");
  CHECK(row->column_text(3) == "https://example.invalid/DEMO-1");
  // `mirror` and `ok` are HARDCODED, not parameters.
  CHECK(row->column_text(4) == "mirror");
  CHECK(row->column_text(5) == "two-way");
  CHECK(row->column_text(6) == "ok");

  // THE POINT OF THE FUNCTION. `link::create` writes external_links and
  // NOTHING else, so an implementation that delegated to it would satisfy
  // every check above and fail only here.
  auto event = conn.prepare("select direction, outcome, fields_changed is null, detail is null "
                            "from sync_events where link_id = ?");
  REQUIRE(event.has_value());
  REQUIRE(event->bind_int64(1, *link_id).has_value());
  auto event_step = event->step();
  REQUIRE(event_step.has_value());
  REQUIRE(*event_step == planar::db::step_result::row);
  CHECK(event->column_text(0) == "push");
  CHECK(event->column_text(1) == "ok");
  CHECK(event->column_int64(2) == 1);
  CHECK(event->column_int64(3) == 1);

  // The mirror it wrote is exactly what `load_existing_mirror` reads back —
  // the two functions are each other's round trip.
  auto const read_back = link::load_existing_mirror(conn, "plan", 4, system_id);
  REQUIRE(read_back.has_value());
  CHECK(*read_back == "DEMO-1");
}

TEST_CASE("record_mirror_link stores an EMPTY url as SQL NULL", "[engine][external][link][mirror]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");

  // GitHub may answer with no `html_url`; the oracle stores NULL rather than
  // the empty string, and `show`/`list` then report it as unset.
  auto const link_id = link::record_mirror_link(conn, "task", 2, system_id, "owner/repo#5", "", link::sync_direction::read_only);
  REQUIRE(link_id.has_value());

  auto row = conn.prepare("select external_url is null from external_links where id = ?");
  REQUIRE(row.has_value());
  REQUIRE(row->bind_int64(1, *link_id).has_value());
  auto stepped = row->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  CHECK(row->column_int64(0) == 1);

  // The PRESENT case, so the assertion above cannot pass because the column
  // is always null.
  auto const with_url = link::record_mirror_link(conn, "task", 3, system_id, "owner/repo#6", "https://x.invalid/6",
                                                 link::sync_direction::read_only);
  REQUIRE(with_url.has_value());
  auto row2 = conn.prepare("select external_url is null from external_links where id = ?");
  REQUIRE(row2.has_value());
  REQUIRE(row2->bind_int64(1, *with_url).has_value());
  auto stepped2 = row2->step();
  REQUIRE(stepped2.has_value());
  REQUIRE(*stepped2 == planar::db::step_result::row);
  CHECK(row2->column_int64(0) == 0);
}

TEST_CASE("record_mirror_link refuses a duplicate and leaves no orphan event", "[engine][external][link][mirror]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");

  auto const first = link::record_mirror_link(conn, "plan", 1, system_id, "DEMO-9", "", link::sync_direction::two_way);
  REQUIRE(first.has_value());

  auto const again = link::record_mirror_link(conn, "plan", 1, system_id, "DEMO-9", "", link::sync_direction::two_way);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == link::link_error::link_exists);

  // The transaction rolled back, so the failed attempt contributed no
  // sync_events row. Exactly one link, exactly one event.
  auto count = conn.prepare("select (select count(*) from external_links), (select count(*) from sync_events)");
  REQUIRE(count.has_value());
  auto counted = count->step();
  REQUIRE(counted.has_value());
  REQUIRE(*counted == planar::db::step_result::row);
  CHECK(count->column_int64(0) == 1);
  CHECK(count->column_int64(1) == 1);
}

// ---- strategy stickiness / verify-counterparts (task 6428) -----------------

auto insert_plan(planar::db::connection& conn, std::int64_t id, std::string_view title, std::string_view slug,
                 std::optional<std::int64_t> parent_plan_id = std::nullopt) -> void {
  auto stmt = conn.prepare("insert into plans (id, scope_kind, title, slug, parent_plan_id) values (?, 'global', ?, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, id).has_value());
  REQUIRE(stmt->bind_text(2, title).has_value());
  REQUIRE(stmt->bind_text(3, slug).has_value());
  if (parent_plan_id.has_value()) {
    REQUIRE(stmt->bind_int64(4, *parent_plan_id).has_value());
  } else {
    REQUIRE(stmt->bind_null(4).has_value());
  }
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
}

TEST_CASE("read_cached_strategy returns unset when no mirror link row exists", "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  auto const cached = link::read_cached_strategy(conn, 99, 1);
  REQUIRE(cached.has_value());
  CHECK_FALSE(cached->has_value());
}

TEST_CASE("read_cached_strategy returns the cached value when config_json carries the strategy key",
          "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");
  insert_plan(conn, 1, "Anchor", "anchor");
  {
    auto stmt = conn.prepare("insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, "
                             "sync_direction, last_sync_status, config_json) "
                             "values ('plan', 1, ?, 'ext-1', 'mirror', 'read-only', 'ok', "
                             "'{\"strategy\":\"github-tracking-issue\",\"extra\":\"keep\"}')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, system_id).has_value());
    REQUIRE(stmt->step().has_value());
  }

  auto const cached = link::read_cached_strategy(conn, 1, system_id);
  REQUIRE(cached.has_value());
  REQUIRE(cached->has_value());
  CHECK(**cached == "github-tracking-issue");
}

TEST_CASE("read_cached_strategy treats an unparseable or keyless config_json as no cache",
          "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");
  insert_plan(conn, 1, "Anchor", "anchor");
  {
    auto stmt = conn.prepare("insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, "
                             "sync_direction, last_sync_status, config_json) "
                             "values ('plan', 1, ?, 'ext-1', 'mirror', 'read-only', 'ok', 'not json')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, system_id).has_value());
    REQUIRE(stmt->step().has_value());
  }

  auto const cached = link::read_cached_strategy(conn, 1, system_id);
  REQUIRE(cached.has_value());
  CHECK_FALSE(cached->has_value());
}

TEST_CASE("list_mirror_links_in_tree walks plan descendants but NOT tasks attached only via tasks.plan_id",
          "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");
  insert_plan(conn, 1, "Anchor", "anchor");
  insert_plan(conn, 2, "Child", "child", 1);
  REQUIRE(link::record_mirror_link(conn, "plan", 1, system_id, "gh#1", "", link::sync_direction::read_only).has_value());
  REQUIRE(link::record_mirror_link(conn, "plan", 2, system_id, "gh#2", "", link::sync_direction::read_only).has_value());

  // A task attached via entity_links(derives-from) IS reachable...
  {
    auto stmt = conn.prepare("insert into tasks (id, scope_kind, title) values (1, 'global', 't1')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    auto link_stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                                  "values ('task', 1, 'plan', 2, 'derives-from')");
    REQUIRE(link_stmt.has_value());
    REQUIRE(link_stmt->step().has_value());
  }
  REQUIRE(link::record_mirror_link(conn, "task", 1, system_id, "gh#3", "", link::sync_direction::read_only).has_value());

  // ...but a second task attached ONLY via tasks.plan_id (no entity_links
  // row) is NOT -- this is the oracle-inherited asymmetry `ext propagate`'s
  // CLI bridge documents (propagate.cpp's happy-path walk uses BOTH paths;
  // this function, ported verbatim from strategy.zig's
  // `listMirrorLinksInTree`, follows only entity_links).
  {
    auto stmt = conn.prepare("insert into tasks (id, scope_kind, plan_id, title) values (2, 'global', 2, 't2')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
  }
  REQUIRE(link::record_mirror_link(conn, "task", 2, system_id, "gh#4", "", link::sync_direction::read_only).has_value());

  auto const links = link::list_mirror_links_in_tree(conn, 1, system_id);
  REQUIRE(links.has_value());
  CHECK(links->size() == 3);
  for (auto const& l : *links) {
    CHECK(l.external_id != "gh#4");
  }
}

TEST_CASE("record_counterpart_missing writes the audit event and deletes the link only when asked",
          "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");
  insert_plan(conn, 1, "Anchor", "anchor");
  auto const link_id = link::record_mirror_link(conn, "plan", 1, system_id, "gh#1", "", link::sync_direction::read_only);
  REQUIRE(link_id.has_value());

  // Record-only: the link survives.
  REQUIRE(link::record_counterpart_missing(conn, *link_id, "plan", 1, "gh#1", false).has_value());
  {
    auto stmt = conn.prepare("select count(*) from external_links where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, *link_id).has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 1);
  }
  {
    auto stmt = conn.prepare("select count(*) from sync_events where outcome = 'counterpart-missing'");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 1);
  }

  // With unlink_or_recreate: the link goes away, a second event is written.
  REQUIRE(link::record_counterpart_missing(conn, *link_id, "plan", 1, "gh#1", true).has_value());
  {
    auto stmt = conn.prepare("select count(*) from external_links where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, *link_id).has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 0);
  }
  {
    auto stmt = conn.prepare("select count(*) from sync_events where outcome = 'counterpart-missing'");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 2);
  }
}

TEST_CASE("abandon_counterparts deletes every mirror link in the subtree and records one event per link",
          "[engine][external][link][strategy]") {
  scratch_db_path const scratch;
  auto                  conn      = open_migrated(scratch);
  auto const            system_id = insert_system(conn, "gh");
  insert_plan(conn, 1, "Anchor", "anchor");
  insert_plan(conn, 2, "Child", "child", 1);
  REQUIRE(link::record_mirror_link(conn, "plan", 1, system_id, "gh#1", "", link::sync_direction::read_only).has_value());
  REQUIRE(link::record_mirror_link(conn, "plan", 2, system_id, "gh#2", "", link::sync_direction::read_only).has_value());

  auto const abandoned = link::abandon_counterparts(conn, 1, system_id, "github-parent-issue", "github-tracking-issue");
  REQUIRE(abandoned.has_value());
  CHECK(*abandoned == 2);

  auto stmt = conn.prepare("select (select count(*) from external_links where system_id = ?), "
                           "(select count(*) from sync_events where outcome = 'strategy-abandoned')");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, system_id).has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  CHECK(stmt->column_int64(0) == 0);
  CHECK(stmt->column_int64(1) == 2);
}
