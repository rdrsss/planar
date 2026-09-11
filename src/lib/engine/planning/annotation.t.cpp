// @file annotation.t.cpp
// @brief Unit tests for `planar.engine.planning.annotation` (plan 996,
// task 6094) -- the fourteen `annotate` schema leaves.
//
// ORACLE PROVENANCE. Every rendered string and every behavioral rule below
// was captured by RUNNING the Zig binary against a scratch database, never
// from `--help` and never from reading the Zig source. The probe sessions,
// reproducible verbatim:
//
// (A) CRUD / lifecycle / rendering -- PLANAR_DB=/tmp/oracle/p2.db
//   $Z annotate add --anchor-path a.txt --title A1 --body b1 --tags "x, y ,x" --json
//     -> {"id":1,"scope_kind":"global","scope_id":null,"anchor":{"path":"a.txt",
//         "line_start":null,"line_end":null,"commit_sha":"","text_hash":"","text":""},
//         "title":"A1","slug":null,"body":"b1","status":"active","vendor":"",
//         "plan_id":null,"task_id":null,"tags":["x","y"],
//         "created_at":"2026-08-23T01:01:48.528Z","updated_at":"2026-08-23T01:01:48.528Z"}
//        [note: "x, y ,x" collapsed to ["x","y"] -- trimmed and de-duplicated]
//   $Z annotate show 1        -> the text block pinned in [render-text] below
//   $Z annotate list          ->     1  active      A1
//                                    2  active      A2
//                                    3  active      A3
//   $Z annotate list --anchor-path nothing.txt --json -> []
//   $Z annotate list --anchor-path nothing.txt        -> (no annotations)
//   $Z annotate tag 1 zz --json          -> {"ok":true,"id":1,"tag":"zz","action":"add"}
//   $Z annotate tag 1 zz --remove --json -> {"ok":true,"id":1,"tag":"zz","action":"remove"}
//   $Z annotate tag 1 qq                 -> annotation 1: added tag 'qq'
//   $Z annotate tag 1 qq --remove        -> annotation 1: removed tag 'qq'
//   $Z annotate remove 3 --json          -> {"ok":true,"id":3}
//   $Z annotate remove 1                 -> annotation 1 removed
//   $Z annotate remove 3 (again)         -> exit 1, error: no annotation with id 3
//   $Z annotate resolve 2 --json         -> exit 0, status becomes "resolved"
//   $Z annotate resolve 2 --json (again) -> exit 0, updated_at BUMPS      <-- identity
//   $Z annotate dismiss 2 --json         -> exit 1, error: annotate dismiss: TerminalStatus
//   $Z annotate archive 2 --json         -> exit 0, status becomes "archived"
//   $Z annotate archive 2 --json (again) -> exit 0, updated_at BUMPS      <-- identity
//
// (B) bulk / sweep / verify -- PLANAR_DB=/tmp/oracle/pb.db
//   $Z annotate bulk-resolve  --anchor-path z.txt --json -> {"ok":true,"action":"resolved","count":3}
//   $Z annotate bulk-resolve  --anchor-path z.txt        -> resolved: 0 annotation(s)
//   $Z annotate bulk-dismiss  --anchor-path z.txt --json -> {"ok":true,"action":"dismissed","count":0}
//   $Z annotate bulk-archive  --anchor-path z.txt --json -> {"ok":true,"action":"archived","count":3}
//        [the three rows were RESOLVED, and bulk-archive still moved all
//         three -- resolved -> archived is legal, so archive does NOT
//         pre-skip outcome states the way resolve/dismiss do]
//   $Z annotate sweep --json      -> {"ok":true,"action":"sweep","since_days":30,"swept":0}
//   $Z annotate sweep             -> sweep: archived 0 annotation(s) older than 30 day(s)
//   $Z annotate verify --json     -> {"ok":true,"rows":[{"id":1,"anchor_path":"z.txt","state":"stale"},
//                                     ...,{"id":4,"anchor_path":"h.txt","state":"fresh"},
//                                     {"id":5,"anchor_path":"h.txt","state":"drifted"},
//                                     {"id":6,"anchor_path":"missing.txt","state":"stale"}]}
//   $Z annotate verify            -> annotation:1  z.txt  [stale]     (one line per row)
//   $Z annotate verify --anchor-path nothing.txt -> (no active annotations)
//        [id 4's --text-hash was `shasum -a 256 h.txt` for a file holding
//         "hello\n" == 5891b5b5...be03, and it classified fresh -- that is
//         how SHA-256-of-the-whole-file-body was established]
//
// (C) HAZARD 1, the bulk transactional boundary -- PLANAR_DB=/tmp/oracle/p3.db
//   four active annotations on z.txt, ids 1..4, then:
//   sqlite3 p3.db "create trigger boom before update on annotations
//                  when new.id = 3 begin select raise(abort,'boom'); end;"
//   $Z annotate bulk-archive --anchor-path z.txt --json
//     -> exit 1, error: annotate bulk-archive: QueryFailed
//   sqlite3 p3.db "select id,status,updated_at from annotations order by id"
//     -> 1|archived|2026-08-23T01:02:11.569Z
//        2|archived|2026-08-23T01:02:11.570Z
//        3|active  |2026-08-23T01:02:11.487Z
//        4|active  |2026-08-23T01:02:11.518Z
//   ANSWER: NOT transactional. The prefix stays applied, the failing row
//   and everything after it are untouched, and the distinct per-row
//   updated_at values confirm one UPDATE per row rather than a set-update.
//   Reproduced against this port in [hazard-bulk-boundary] below.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.annotation;
import planar.engine.planning.transitions;

namespace {

namespace ann = planar::engine::planning::annotation;

using planar::engine::planning::check_transition;
using planar::engine::planning::transition_error;
using planar::engine::planning::transition_kind;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_annotation_test_{}_{}.db",
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

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
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

auto add_simple(planar::db::connection& conn, std::string_view path, std::string_view title) -> std::int64_t {
  auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = std::string{path}}, .title = title});
  REQUIRE(created.has_value());
  return created->id;
}

auto insert_assoc(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn, std::format("insert into associations (slug, name, kind) values ('{}', '{}', 'org')", slug, slug));
  return scalar_int(conn, std::format("select id from associations where slug = '{}'", slug));
}

auto insert_plan(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn,
       std::format("insert into plans (scope_kind, title, slug, status) values ('global', '{}', '{}', 'draft')", slug, slug));
  return scalar_int(conn, std::format("select id from plans where slug = '{}'", slug));
}

auto insert_task(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority) values ('global', '{}', 'todo', 100)", title));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

// The `--tags "x, y ,x"` fixture, spelled the way the CLI splits it before
// handing the pieces to the engine.
auto tags_of(std::initializer_list<std::string_view> raw) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto s : raw) {
    out.emplace_back(s);
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// annotate add / show
// ---------------------------------------------------------------------------

TEST_CASE("create stores an annotation with active status and a global scope by default", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .title = "A1", .body = "b1"});
  REQUIRE(created.has_value());
  CHECK(created->id > 0);
  CHECK(created->status_ == ann::status::active);
  CHECK(created->scope_kind_ == ann::scope_kind::global);
  CHECK_FALSE(created->scope_id.has_value());
  CHECK(created->anchor.path == "a.txt");
  REQUIRE(created->title.has_value());
  CHECK(*created->title == "A1");
  CHECK(created->body == "b1");
  CHECK(created->vendor.empty());
  CHECK_FALSE(created->slug.has_value());
  CHECK(created->tags.empty());
}

TEST_CASE("entity annotations have an exact target scope and no file path", "[annotation][entity-anchor]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      plan = insert_plan(conn, "entity-target");

  auto created = ann::create(conn, ann::create_args{
                                       .anchor       = {},
                                       .anchor_kind_ = ann::anchor_kind::entity,
                                       .target       = ann::entity_target{.kind = ann::target_kind::plan, .id = plan},
                                       .body         = "durable page note",
                                   });
  REQUIRE(created.has_value());
  CHECK(created->anchor_kind_ == ann::anchor_kind::entity);
  REQUIRE(created->target.has_value());
  CHECK(created->target->kind == ann::target_kind::plan);
  CHECK(created->target->id == plan);
  CHECK(created->anchor.path.empty());
  CHECK(created->revision == 1);
  CHECK(scalar_int(conn, "select count(*) from annotations where anchor_kind = 'entity' and anchor_path is null") == 1);

  auto wrong_scope = ann::create(conn, ann::create_args{
                                           .anchor       = {},
                                           .anchor_kind_ = ann::anchor_kind::entity,
                                           .target       = ann::entity_target{.kind = ann::target_kind::plan, .id = plan},
                                           .scope        = std::string_view{"missing-scope"},
                                       });
  CHECK_FALSE(wrong_scope.has_value());
  CHECK(wrong_scope.error() == ann::annotation_error::slug_not_found);

  exec(conn, "update annotations set status = 'resolved', updated_at = '2000-01-01T00:00:00.000Z' where id = 1");
  auto swept = ann::sweep(conn, 1, std::nullopt);
  REQUIRE(swept.has_value());
  CHECK(*swept == 0);
  CHECK(scalar_text(conn, "select status from annotations where id = 1") == "resolved");
}

TEST_CASE("create trims and de-duplicates tags", "[annotation][parity]") {
  // Oracle: `--tags "x, y ,x"` stored exactly ["x","y"].
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"},
                                                    .title  = "A1",
                                                    .tags   = tags_of({"x", " y ", "x", "  ", "\tz\n"})});
  REQUIRE(created.has_value());
  REQUIRE(created->tags.size() == 3);
  CHECK(created->tags[0] == "x");
  CHECK(created->tags[1] == "y");
  CHECK(created->tags[2] == "z");
}

TEST_CASE("an unset line range stores SQL NULL, not zero", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "file-level");

  CHECK(scalar_int(conn, std::format("select count(*) from annotations where id = {} "
                                     "and anchor_line_start is null and anchor_line_end is null",
                                     id)) == 1);
}

TEST_CASE("a line range round-trips", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = ann::create(
      conn, ann::create_args{.anchor = ann::anchor_fields{.path = "b.txt", .line_start = 3, .line_end = 9}, .title = "A3"});
  REQUIRE(created.has_value());
  REQUIRE(created->anchor.line_start.has_value());
  CHECK(*created->anchor.line_start == 3);
  REQUIRE(created->anchor.line_end.has_value());
  CHECK(*created->anchor.line_end == 9);
}

TEST_CASE("show reports not_found for a missing id", "[annotation]") {
  // Oracle: `annotate show 999 --json` -> exit 1, `error: no annotation with id 999`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = ann::show(conn, 999);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::not_found);
}

TEST_CASE("show_by_slug resolves, and reports not_found for an unknown slug", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .slug = "my-slug"});
  REQUIRE(created.has_value());

  auto found = ann::show_by_slug(conn, "my-slug");
  REQUIRE(found.has_value());
  CHECK(found->id == created->id);

  auto missing = ann::show_by_slug(conn, "nope");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == ann::annotation_error::not_found);
}

TEST_CASE("a duplicate slug is refused with slug_conflict", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .slug = "dup"}).has_value());

  auto second = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "b.txt"}, .slug = "dup"});
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error() == ann::annotation_error::slug_conflict);
}

TEST_CASE("create resolves an association scope and refuses an unknown slug", "[annotation]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "acme");

  auto scoped = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .scope = "acme"});
  REQUIRE(scoped.has_value());
  CHECK(scoped->scope_kind_ == ann::scope_kind::association);
  REQUIRE(scoped->scope_id.has_value());
  CHECK(*scoped->scope_id == assoc_id);

  auto bad = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .scope = "nope"});
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error() == ann::annotation_error::slug_not_found);
}

TEST_CASE("an explicit scope of global stores global with a null scope_id", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .scope = "global"});
  REQUIRE(created.has_value());
  CHECK(created->scope_kind_ == ann::scope_kind::global);
  CHECK_FALSE(created->scope_id.has_value());
}

// ---------------------------------------------------------------------------
// annotate list
// ---------------------------------------------------------------------------

TEST_CASE("list returns everything in id order when unfiltered", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      a    = add_simple(conn, "a.txt", "A1");
  const auto      b    = add_simple(conn, "a.txt", "A2");
  const auto      c    = add_simple(conn, "b.txt", "A3");

  auto items = ann::list(conn, ann::list_filter{});
  REQUIRE(items.has_value());
  REQUIRE(items->size() == 3);
  CHECK((*items)[0].id == a);
  CHECK((*items)[1].id == b);
  CHECK((*items)[2].id == c);
}

TEST_CASE("list filters by anchor_path, status and vendor independently", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "a.txt", "A1");
  const auto b = add_simple(conn, "b.txt", "A2");
  REQUIRE(ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "c.txt"}, .vendor = "claude"}).has_value());
  REQUIRE(ann::resolve(conn, b).has_value());

  auto by_path = ann::list(conn, ann::list_filter{.anchor_path = "b.txt"});
  REQUIRE(by_path.has_value());
  REQUIRE(by_path->size() == 1);
  CHECK((*by_path)[0].id == b);

  auto by_status = ann::list(conn, ann::list_filter{.status_ = ann::status::resolved});
  REQUIRE(by_status.has_value());
  REQUIRE(by_status->size() == 1);
  CHECK((*by_status)[0].id == b);

  auto by_vendor = ann::list(conn, ann::list_filter{.vendor = "claude"});
  REQUIRE(by_vendor.has_value());
  REQUIRE(by_vendor->size() == 1);
  CHECK((*by_vendor)[0].vendor == "claude");
}

TEST_CASE("list filters by plan_id and task_id", "[annotation]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = insert_plan(conn, "p1");
  const auto      task_id = insert_task(conn, "T");
  add_simple(conn, "a.txt", "unbound");
  auto on_plan = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "b.txt"}, .plan_id = plan_id});
  auto on_task = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "c.txt"}, .task_id = task_id});
  REQUIRE(on_plan.has_value());
  REQUIRE(on_task.has_value());

  auto by_plan = ann::list(conn, ann::list_filter{.plan_id = plan_id});
  REQUIRE(by_plan.has_value());
  REQUIRE(by_plan->size() == 1);
  CHECK((*by_plan)[0].id == on_plan->id);

  auto by_task = ann::list(conn, ann::list_filter{.task_id = task_id});
  REQUIRE(by_task.has_value());
  REQUIRE(by_task->size() == 1);
  CHECK((*by_task)[0].id == on_task->id);
}

TEST_CASE("list filters by tag", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            tagged =
      ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .tags = tags_of({"perf", "todo"})});
  REQUIRE(tagged.has_value());
  REQUIRE(
      ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "b.txt"}, .tags = tags_of({"todo"})}).has_value());

  auto perf = ann::list(conn, ann::list_filter{.tag = "perf"});
  REQUIRE(perf.has_value());
  REQUIRE(perf->size() == 1);
  CHECK((*perf)[0].id == tagged->id);

  auto todo = ann::list(conn, ann::list_filter{.tag = "todo"});
  REQUIRE(todo.has_value());
  CHECK(todo->size() == 2);
}

TEST_CASE("list filters by scope", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "acme");
  add_simple(conn, "a.txt", "global one");
  auto scoped = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "b.txt"}, .scope = "acme"});
  REQUIRE(scoped.has_value());

  auto in_assoc = ann::list(conn, ann::list_filter{.scope = "acme"});
  REQUIRE(in_assoc.has_value());
  REQUIRE(in_assoc->size() == 1);
  CHECK((*in_assoc)[0].id == scoped->id);

  auto in_global = ann::list(conn, ann::list_filter{.scope = "global"});
  REQUIRE(in_global.has_value());
  REQUIRE(in_global->size() == 1);
  CHECK((*in_global)[0].anchor.path == "a.txt");
}

TEST_CASE("list combines filters conjunctively", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      want = add_simple(conn, "a.txt", "want");
  add_simple(conn, "a.txt", "wrong status");
  add_simple(conn, "b.txt", "wrong path");
  REQUIRE(ann::resolve(conn, want).has_value());

  auto items = ann::list(conn, ann::list_filter{.anchor_path = "a.txt", .status_ = ann::status::resolved});
  REQUIRE(items.has_value());
  REQUIRE(items->size() == 1);
  CHECK((*items)[0].id == want);
}

// ---------------------------------------------------------------------------
// annotate update
// ---------------------------------------------------------------------------

TEST_CASE("update patches only the named fields", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            created =
      ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .title = "A2", .body = "keep me"});
  REQUIRE(created.has_value());

  auto patched = ann::update(conn, created->id, ann::update_args{.title = "T2b"});
  REQUIRE(patched.has_value());
  REQUIRE(patched->title.has_value());
  CHECK(*patched->title == "T2b");
  CHECK(patched->body == "keep me");
  CHECK(patched->anchor.path == "a.txt");
}

TEST_CASE("an all-unset patch is a no-op that does NOT bump updated_at", "[annotation][parity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  exec(conn, std::format("update annotations set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", id));

  auto patched = ann::update(conn, id, ann::update_args{});
  REQUIRE(patched.has_value());
  CHECK(patched->updated_at == "2000-01-01T00:00:00.000Z");
}

TEST_CASE("a non-empty patch DOES bump updated_at", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  exec(conn, std::format("update annotations set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", id));

  auto patched = ann::update(conn, id, ann::update_args{.title = "B"});
  REQUIRE(patched.has_value());
  CHECK(patched->updated_at != "2000-01-01T00:00:00.000Z");
}

TEST_CASE("update replaces the whole anchor descriptor at once", "[annotation]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            created = ann::create(
      conn,
      ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt", .line_start = 1, .line_end = 2, .commit_sha = "old-sha"},
                       .title  = "A"});
  REQUIRE(created.has_value());

  auto patched =
      ann::update(conn, created->id, ann::update_args{.anchor = ann::anchor_fields{.path = "moved.txt", .commit_sha = "new"}});
  REQUIRE(patched.has_value());
  CHECK(patched->anchor.path == "moved.txt");
  CHECK(patched->anchor.commit_sha == "new");
  // The unset halves of the replacement anchor CLEAR the stored values --
  // it is a whole-descriptor replace, not a merge.
  CHECK_FALSE(patched->anchor.line_start.has_value());
  CHECK_FALSE(patched->anchor.line_end.has_value());
}

TEST_CASE("update moves an annotation to an association scope", "[annotation]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "acme");
  const auto      id       = add_simple(conn, "a.txt", "A");

  auto patched = ann::update(conn, id, ann::update_args{.scope = "acme"});
  REQUIRE(patched.has_value());
  CHECK(patched->scope_kind_ == ann::scope_kind::association);
  REQUIRE(patched->scope_id.has_value());
  CHECK(*patched->scope_id == assoc_id);
}

TEST_CASE("update validates a status change against the transition matrix", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  REQUIRE(ann::update(conn, id, ann::update_args{.status_ = ann::status::resolved}).has_value());

  auto illegal = ann::update(conn, id, ann::update_args{.status_ = ann::status::dismissed});
  REQUIRE_FALSE(illegal.has_value());
  CHECK(illegal.error() == ann::annotation_error::terminal_status);
}

TEST_CASE("update of a missing annotation is not_found", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = ann::update(conn, 999, ann::update_args{.title = "x"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::not_found);
}

TEST_CASE("update to an already-taken slug is slug_conflict", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .slug = "taken"}).has_value());
  const auto other = add_simple(conn, "b.txt", "B");

  auto res = ann::update(conn, other, ann::update_args{.slug = "taken"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::slug_conflict);
}

// ---------------------------------------------------------------------------
// annotate remove
// ---------------------------------------------------------------------------

TEST_CASE("remove deletes the row and cascades to its tags", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            created =
      ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = "a.txt"}, .tags = tags_of({"one", "two"})});
  REQUIRE(created.has_value());
  CHECK(scalar_int(conn, "select count(*) from annotation_tags") == 2);

  REQUIRE(ann::remove(conn, created->id).has_value());
  CHECK(scalar_int(conn, "select count(*) from annotations") == 0);
  CHECK(scalar_int(conn, "select count(*) from annotation_tags") == 0);
}

TEST_CASE("removing a missing annotation is not_found, not a silent no-op", "[annotation]") {
  // Oracle: the second `annotate remove 3` exits 1 with
  // `error: no annotation with id 3`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  REQUIRE(ann::remove(conn, id).has_value());

  auto again = ann::remove(conn, id);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == ann::annotation_error::not_found);
}

// ---------------------------------------------------------------------------
// annotate resolve / dismiss / archive
// ---------------------------------------------------------------------------

TEST_CASE("active moves to each of the three outcome states", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      a    = add_simple(conn, "a.txt", "A");
  const auto      b    = add_simple(conn, "b.txt", "B");
  const auto      c    = add_simple(conn, "c.txt", "C");

  auto r = ann::resolve(conn, a);
  REQUIRE(r.has_value());
  CHECK(r->status_ == ann::status::resolved);

  auto d = ann::dismiss(conn, b);
  REQUIRE(d.has_value());
  CHECK(d->status_ == ann::status::dismissed);

  auto x = ann::archive(conn, c);
  REQUIRE(x.has_value());
  CHECK(x->status_ == ann::status::archived);
}

TEST_CASE("resolved and dismissed still progress to archived", "[annotation]") {
  // The retention-tier model (plan 692), confirmed by the oracle: three
  // RESOLVED rows were all moved by `bulk-archive` (count 3).
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      a    = add_simple(conn, "a.txt", "A");
  const auto      b    = add_simple(conn, "b.txt", "B");
  REQUIRE(ann::resolve(conn, a).has_value());
  REQUIRE(ann::dismiss(conn, b).has_value());

  auto from_resolved = ann::archive(conn, a);
  REQUIRE(from_resolved.has_value());
  CHECK(from_resolved->status_ == ann::status::archived);

  auto from_dismissed = ann::archive(conn, b);
  REQUIRE(from_dismissed.has_value());
  CHECK(from_dismissed->status_ == ann::status::archived);
}

TEST_CASE("a lateral move between outcome states is refused", "[annotation][parity]") {
  // Oracle: `annotate dismiss 2` on a RESOLVED row exits 1 with
  // `error: annotate dismiss: TerminalStatus`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  REQUIRE(ann::resolve(conn, id).has_value());

  auto res = ann::dismiss(conn, id);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::terminal_status);
}

TEST_CASE("archived is the sole final state -- no outgoing edges", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  REQUIRE(ann::archive(conn, id).has_value());

  auto r = ann::resolve(conn, id);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == ann::annotation_error::terminal_status);

  auto d = ann::dismiss(conn, id);
  REQUIRE_FALSE(d.has_value());
  CHECK(d.error() == ann::annotation_error::terminal_status);
}

TEST_CASE("an identity transition SUCCEEDS and still bumps updated_at", "[annotation][parity]") {
  // Oracle, and NOT the intuitive outcome: `annotate resolve 2` twice both
  // exited 0, and the second call's response carried a LATER updated_at
  // (...:48.763Z then ...:48.782Z). Same for `archive` twice
  // (...:48.820Z then ...:48.839Z). The identity check short-circuits the
  // MATRIX, not the UPDATE -- the row is rewritten with a fresh timestamp.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");
  REQUIRE(ann::resolve(conn, id).has_value());
  exec(conn, std::format("update annotations set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", id));

  auto again = ann::resolve(conn, id);
  REQUIRE(again.has_value());
  CHECK(again->status_ == ann::status::resolved);
  CHECK(again->updated_at != "2000-01-01T00:00:00.000Z");
}

TEST_CASE("transitioning a missing annotation is not_found, not terminal_status", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = ann::resolve(conn, 999);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::not_found);
}

TEST_CASE("the annotation transition arm matches the retention-tier matrix", "[annotation][transitions]") {
  using ann::status_to_text;
  const auto legal = [](std::string_view from, std::string_view to) {
    return check_transition(transition_kind::annotation, from, to, false).has_value();
  };

  CHECK(legal("active", "resolved"));
  CHECK(legal("active", "dismissed"));
  CHECK(legal("active", "archived"));
  CHECK(legal("resolved", "archived"));
  CHECK(legal("dismissed", "archived"));

  CHECK_FALSE(legal("resolved", "active"));
  CHECK_FALSE(legal("resolved", "dismissed"));
  CHECK_FALSE(legal("dismissed", "active"));
  CHECK_FALSE(legal("dismissed", "resolved"));
  CHECK_FALSE(legal("archived", "active"));
  CHECK_FALSE(legal("archived", "resolved"));
  CHECK_FALSE(legal("archived", "dismissed"));

  // Identity is always a no-op success, for every status.
  CHECK(legal("active", "active"));
  CHECK(legal("resolved", "resolved"));
  CHECK(legal("dismissed", "dismissed"));
  CHECK(legal("archived", "archived"));

  // An unrecognized `from` is unknown_status, distinct from a refusal.
  auto unknown = check_transition(transition_kind::annotation, "open", "resolved", false);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error() == transition_error::unknown_status);
}

TEST_CASE("force does NOT bypass the annotation matrix", "[annotation][transitions]") {
  // transitions.cppm:74 asserts this in prose ("`force` has no effect on this
  // arm -- the annotate verbs expose no `--force`") and transitions.cpp
  // implements it by scoping the bypass to `kind == transition_kind::task`.
  // Nothing pinned it: every other annotation caller passes force=false, so
  // relaxing that guard to a bare `if (force)` broke no test (review finding
  // F5). It is pinned BEFORE layer 3 exists, because the day a `cmd_*` handler
  // does thread a `--force` through, the regression is silent -- `archived`,
  // the SOLE final state of the retention-tier model, quietly reopens.
  const auto forced = [](std::string_view from, std::string_view to) {
    return check_transition(transition_kind::annotation, from, to, true).has_value();
  };

  // Every edge the matrix REFUSES stays refused under force=true.
  CHECK_FALSE(forced("resolved", "active"));
  CHECK_FALSE(forced("resolved", "dismissed"));
  CHECK_FALSE(forced("dismissed", "active"));
  CHECK_FALSE(forced("dismissed", "resolved"));
  CHECK_FALSE(forced("archived", "active"));
  CHECK_FALSE(forced("archived", "resolved"));
  CHECK_FALSE(forced("archived", "dismissed"));

  // And the error is still a REFUSAL, not a masked unknown_status.
  auto refused = check_transition(transition_kind::annotation, "archived", "active", true);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == transition_error::illegal_transition);

  // force=true does not break the legal edges either -- it is inert, not
  // inverted. (Identity short-circuits ahead of the kind check, so it is
  // unaffected by definition and is not re-asserted here.)
  CHECK(forced("active", "resolved"));
  CHECK(forced("resolved", "archived"));

  // The contrast that makes the scoping visible: on the TASK arm the very
  // same flag DOES bypass, so this is a per-kind rule rather than force being
  // a no-op everywhere.
  CHECK_FALSE(check_transition(transition_kind::task, "done", "todo", false).has_value());
  CHECK(check_transition(transition_kind::task, "done", "todo", true).has_value());
}

TEST_CASE("is_terminal covers the three outcome states, not just archived", "[annotation]") {
  CHECK_FALSE(ann::is_terminal(ann::status::active));
  CHECK(ann::is_terminal(ann::status::resolved));
  CHECK(ann::is_terminal(ann::status::dismissed));
  CHECK(ann::is_terminal(ann::status::archived));
}

// ---------------------------------------------------------------------------
// annotate tag
// ---------------------------------------------------------------------------

TEST_CASE("add_tag, remove_tag and list_tags round-trip", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");

  REQUIRE(ann::add_tag(conn, id, "zebra").has_value());
  REQUIRE(ann::add_tag(conn, id, "alpha").has_value());

  auto tags = ann::list_tags(conn, id);
  REQUIRE(tags.has_value());
  REQUIRE(tags->size() == 2);
  // Lexicographic ascending, not insertion order.
  CHECK((*tags)[0] == "alpha");
  CHECK((*tags)[1] == "zebra");

  REQUIRE(ann::remove_tag(conn, id, "alpha").has_value());
  auto after = ann::list_tags(conn, id);
  REQUIRE(after.has_value());
  REQUIRE(after->size() == 1);
  CHECK((*after)[0] == "zebra");
}

TEST_CASE("add_tag trims and absorbs duplicates", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");

  REQUIRE(ann::add_tag(conn, id, "  spaced  ").has_value());
  REQUIRE(ann::show(conn, id)->revision == 2);
  REQUIRE(ann::add_tag(conn, id, "spaced").has_value());
  CHECK(ann::show(conn, id)->revision == 2);

  auto tags = ann::list_tags(conn, id);
  REQUIRE(tags.has_value());
  REQUIRE(tags->size() == 1);
  CHECK((*tags)[0] == "spaced");
}

TEST_CASE("an empty tag is refused on both tag verbs", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");

  auto add = ann::add_tag(conn, id, "   ");
  REQUIRE_FALSE(add.has_value());
  CHECK(add.error() == ann::annotation_error::empty_tag);

  auto rm = ann::remove_tag(conn, id, "\t\n");
  REQUIRE_FALSE(rm.has_value());
  CHECK(rm.error() == ann::annotation_error::empty_tag);
}

TEST_CASE("add_tag checks the annotation exists but remove_tag does not", "[annotation][parity]") {
  // Asymmetric in the Zig original and preserved here: add_tag calls
  // show() first (so the operator hears not_found rather than getting a
  // dangling-looking row), remove_tag issues a bare DELETE.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto add = ann::add_tag(conn, 999, "t");
  REQUIRE_FALSE(add.has_value());
  CHECK(add.error() == ann::annotation_error::not_found);

  auto rm = ann::remove_tag(conn, 999, "t");
  CHECK(rm.has_value());
}

TEST_CASE("removing a tag that was never attached is a no-op", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id   = add_simple(conn, "a.txt", "A");

  CHECK(ann::remove_tag(conn, id, "never-added").has_value());
  CHECK(ann::show(conn, id)->revision == 1);
  auto tags = ann::list_tags(conn, id);
  REQUIRE(tags.has_value());
  CHECK(tags->empty());
}

// ---------------------------------------------------------------------------
// annotate bulk-resolve / bulk-dismiss / bulk-archive
// ---------------------------------------------------------------------------

TEST_CASE("bulk_apply transitions every matching active row", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "z.txt", "B1");
  add_simple(conn, "z.txt", "B2");
  add_simple(conn, "z.txt", "B3");
  add_simple(conn, "other.txt", "elsewhere");

  auto count =
      ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt", .status_ = ann::status::active}, ann::bulk_action::resolve);
  REQUIRE(count.has_value());
  CHECK(*count == 3);
  CHECK(scalar_int(conn, "select count(*) from annotations where status = 'resolved'") == 3);
  CHECK(scalar_text(conn, "select status from annotations where anchor_path = 'other.txt'") == "active");
}

TEST_CASE("a second bulk-resolve pass counts zero", "[annotation][parity]") {
  // Oracle: the second `annotate bulk-resolve --anchor-path z.txt`
  // printed `resolved: 0 annotation(s)`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "z.txt", "B1");
  add_simple(conn, "z.txt", "B2");
  const ann::list_filter filter{.anchor_path = "z.txt", .status_ = ann::status::active};

  REQUIRE(*ann::bulk_apply(conn, filter, ann::bulk_action::resolve) == 2);
  auto second = ann::bulk_apply(conn, filter, ann::bulk_action::resolve);
  REQUIRE(second.has_value());
  CHECK(*second == 0);
}

TEST_CASE("bulk-dismiss skips rows already at an outcome state", "[annotation][parity]") {
  // Oracle: after bulk-resolve moved all three z.txt rows,
  // `bulk-dismiss --anchor-path z.txt --json` reported count 0.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "z.txt", "B1");
  add_simple(conn, "z.txt", "B2");
  REQUIRE(*ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt", .status_ = ann::status::active},
                           ann::bulk_action::resolve) == 2);

  // bulk-dismiss's own filter pins status=active, so nothing is selected
  // at all here -- which is the CLI's shape.
  auto with_filter =
      ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt", .status_ = ann::status::active}, ann::bulk_action::dismiss);
  REQUIRE(with_filter.has_value());
  CHECK(*with_filter == 0);

  // And even with NO status filter, every resolved row is pre-skipped by
  // the is_terminal guard -- this is the assertion that actually
  // exercises the guard rather than the SQL WHERE clause.
  auto unfiltered = ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt"}, ann::bulk_action::dismiss);
  REQUIRE(unfiltered.has_value());
  CHECK(*unfiltered == 0);
}

TEST_CASE("bulk-archive does NOT pre-skip outcome states", "[annotation][parity]") {
  // The load-bearing asymmetry. Oracle: three RESOLVED z.txt rows, and
  // `bulk-archive --anchor-path z.txt --json` reported count 3.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "z.txt", "B1");
  add_simple(conn, "z.txt", "B2");
  add_simple(conn, "z.txt", "B3");
  REQUIRE(*ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt", .status_ = ann::status::active},
                           ann::bulk_action::resolve) == 3);

  // bulk-archive's CLI filter carries no status at all.
  auto count = ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt"}, ann::bulk_action::archive);
  REQUIRE(count.has_value());
  CHECK(*count == 3);
  CHECK(scalar_int(conn, "select count(*) from annotations where status = 'archived'") == 3);
}

TEST_CASE("bulk-archive skips rows already archived", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_simple(conn, "z.txt", "B1");
  add_simple(conn, "z.txt", "B2");
  REQUIRE(*ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt"}, ann::bulk_action::archive) == 2);

  auto second = ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt"}, ann::bulk_action::archive);
  REQUIRE(second.has_value());
  CHECK(*second == 0);
}

TEST_CASE("bulk_apply rolls back every row when one transition fails", "[annotation][bulk-transaction]") {
  // HAZARD 1, answered by experiment against the oracle and reproduced
  // here with the SAME mechanism (a BEFORE UPDATE trigger that aborts on
  // one specific row). See this file's header, section (C), for the
  // oracle transcript this mirrors.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      id1  = add_simple(conn, "z.txt", "B1");
  const auto      id2  = add_simple(conn, "z.txt", "B2");
  const auto      id3  = add_simple(conn, "z.txt", "B3");
  const auto      id4  = add_simple(conn, "z.txt", "B4");

  exec(conn, std::format("create trigger boom before update on annotations when new.id = {} "
                         "begin select raise(abort,'boom'); end;",
                         id3));

  auto res = ann::bulk_apply(conn, ann::list_filter{.anchor_path = "z.txt"}, ann::bulk_action::archive);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ann::annotation_error::query_failed);

  // The failed invocation has no partial outcome.
  CHECK(scalar_text(conn, std::format("select status from annotations where id = {}", id1)) == "active");
  CHECK(scalar_text(conn, std::format("select status from annotations where id = {}", id2)) == "active");
  CHECK(scalar_text(conn, std::format("select status from annotations where id = {}", id3)) == "active");
  CHECK(scalar_text(conn, std::format("select status from annotations where id = {}", id4)) == "active");
}

TEST_CASE("the bulk envelopes match the oracle byte for byte", "[annotation][parity]") {
  CHECK(ann::render_bulk_json("resolved", 3) == R"({"ok":true,"action":"resolved","count":3})");
  CHECK(ann::render_bulk_json("dismissed", 0) == R"({"ok":true,"action":"dismissed","count":0})");
  CHECK(ann::render_bulk_json("archived", 3) == R"({"ok":true,"action":"archived","count":3})");
  CHECK(ann::render_bulk_text("resolved", 0) == "resolved: 0 annotation(s)");
  CHECK(ann::render_bulk_text("archived", 3) == "archived: 3 annotation(s)");
}

// ---------------------------------------------------------------------------
// annotate sweep
// ---------------------------------------------------------------------------

TEST_CASE("sweep selects only stale resolved and dismissed rows", "[annotation]") {
  scratch_db_path scratch;
  auto            conn           = open_migrated(scratch);
  const auto      old_resolved   = add_simple(conn, "a.txt", "old resolved");
  const auto      old_dismissed  = add_simple(conn, "b.txt", "old dismissed");
  const auto      old_active     = add_simple(conn, "c.txt", "old active");
  const auto      old_archived   = add_simple(conn, "d.txt", "old archived");
  const auto      fresh_resolved = add_simple(conn, "e.txt", "fresh resolved");

  REQUIRE(ann::resolve(conn, old_resolved).has_value());
  REQUIRE(ann::dismiss(conn, old_dismissed).has_value());
  REQUIRE(ann::archive(conn, old_archived).has_value());
  REQUIRE(ann::resolve(conn, fresh_resolved).has_value());

  // Age everything except the "fresh" row well past the cutoff.
  for (const auto id : {old_resolved, old_dismissed, old_active, old_archived}) {
    exec(conn, std::format("update annotations set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", id));
  }

  auto candidates = ann::sweep_candidates(conn, 30);
  REQUIRE(candidates.has_value());
  REQUIRE(candidates->size() == 2);
  CHECK(std::ranges::find(*candidates, old_resolved) != candidates->end());
  CHECK(std::ranges::find(*candidates, old_dismissed) != candidates->end());
  // Active is not an outcome state; archived is already final; the fresh
  // row is inside the cutoff.
  CHECK(std::ranges::find(*candidates, old_active) == candidates->end());
  CHECK(std::ranges::find(*candidates, old_archived) == candidates->end());
  CHECK(std::ranges::find(*candidates, fresh_resolved) == candidates->end());
}

TEST_CASE("sweep archives its candidates and reports the count", "[annotation]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      a    = add_simple(conn, "a.txt", "A");
  const auto      b    = add_simple(conn, "b.txt", "B");
  REQUIRE(ann::resolve(conn, a).has_value());
  REQUIRE(ann::dismiss(conn, b).has_value());
  exec(conn, "update annotations set updated_at = '2000-01-01T00:00:00.000Z'");

  auto swept = ann::sweep(conn, 30);
  REQUIRE(swept.has_value());
  CHECK(*swept == 2);
  CHECK(scalar_int(conn, "select count(*) from annotations where status = 'archived'") == 2);
}

namespace {

/// @brief Seed three eligible sweep candidates -- one in association
/// `acme`, one in association `other`, one global -- backdated past every
/// cutoff the scoped-sweep cases use.
/// @param conn An open, migrated connection.
/// @return The ids, in that order.
auto seed_sweep_scopes(planar::db::connection& conn) -> std::array<std::int64_t, 3> {
  insert_assoc(conn, "acme");
  insert_assoc(conn, "other");

  auto scoped = [&](std::string_view path, std::string_view slug) {
    auto created = ann::create(conn, ann::create_args{.anchor = ann::anchor_fields{.path = std::string{path}}, .scope = slug});
    REQUIRE(created.has_value());
    return created->id;
  };
  const auto in_acme   = scoped("acme.txt", "acme");
  const auto in_other  = scoped("other.txt", "other");
  const auto in_global = add_simple(conn, "global.txt", "global");

  for (const auto id : {in_acme, in_other, in_global}) {
    REQUIRE(ann::resolve(conn, id).has_value());
  }
  // `resolve` stamps updated_at with `now` and the cutoff is a STRICT `>`,
  // so a same-millisecond row would be ineligible even at cutoff 0.
  exec(conn, "update annotations set updated_at = '2000-01-01T00:00:00.000Z'");
  return {in_acme, in_other, in_global};
}

/// @brief Read one annotation's status straight from the table.
/// @param conn An open connection.
/// @param id The annotation id.
/// @return The stored status text.
auto status_text_of(planar::db::connection& conn, std::int64_t id) -> std::string {
  return scalar_text(conn, std::format("select status from annotations where id = {}", id));
}

} // namespace

// This is the arm that pins task 6150. Asserting the swept COUNT would not
// have caught the defect: the broken build reported an accurate count of
// what it archived, and what it archived was every scope's rows. Only the
// survival of the out-of-scope rows separates the two behaviours.
TEST_CASE("a scoped sweep leaves out-of-scope annotations untouched", "[annotation][scope]") {
  scratch_db_path scratch;
  auto            conn                      = open_migrated(scratch);
  auto const [in_acme, in_other, in_global] = seed_sweep_scopes(conn);

  auto swept = ann::sweep(conn, 0, "acme");
  REQUIRE(swept.has_value());

  // Survival FIRST, deliberately: these are the assertions that fail under
  // the pre-6150 behaviour, and they are checked before the count so a
  // regression reports the blast radius rather than an arithmetic mismatch.
  CHECK(status_text_of(conn, in_other) == "resolved");
  CHECK(status_text_of(conn, in_global) == "resolved");
  CHECK(status_text_of(conn, in_acme) == "archived");
  CHECK(*swept == 1);
}

TEST_CASE("a sweep scoped to global reaches only global rows", "[annotation][scope]") {
  scratch_db_path scratch;
  auto            conn                      = open_migrated(scratch);
  auto const [in_acme, in_other, in_global] = seed_sweep_scopes(conn);

  auto swept = ann::sweep(conn, 0, "global");
  REQUIRE(swept.has_value());
  CHECK(*swept == 1);
  CHECK(status_text_of(conn, in_global) == "archived");
  CHECK(status_text_of(conn, in_acme) == "resolved");
  CHECK(status_text_of(conn, in_other) == "resolved");
}

TEST_CASE("an unscoped sweep still spans every scope", "[annotation][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      ids  = seed_sweep_scopes(conn);

  auto swept = ann::sweep(conn, 0);
  REQUIRE(swept.has_value());
  CHECK(*swept == 3);
  for (const auto id : ids) {
    CHECK(status_text_of(conn, id) == "archived");
  }
}

TEST_CASE("a sweep named at an unresolvable scope refuses before archiving anything", "[annotation][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      ids  = seed_sweep_scopes(conn);

  auto swept = ann::sweep(conn, 0, "nosuch");
  REQUIRE_FALSE(swept.has_value());
  CHECK(swept.error() == ann::annotation_error::slug_not_found);
  // The refusal has to land BEFORE the loop, not part-way through it.
  for (const auto id : ids) {
    CHECK(status_text_of(conn, id) == "resolved");
  }

  auto candidates = ann::sweep_candidates(conn, 0, "nosuch");
  REQUIRE_FALSE(candidates.has_value());
  CHECK(candidates.error() == ann::annotation_error::slug_not_found);
}

TEST_CASE("the sweep cutoff selects strictly by age, at the exact day boundary", "[annotation][parity]") {
  // BREAK-PROBE HISTORY, recorded because it changed this test. The first
  // version of this case was named "the sweep cutoff is STRICTLY
  // greater-than" and asserted only the two extremes (`since_days` huge ->
  // nothing; `since_days` 0 -> everything). Mutating the implementation's
  // `>` to `>=` left it PASSING -- it did not discriminate, which is a
  // defect in the test, not a curiosity.
  //
  // It cannot: `>` and `>=` differ only when
  // `julianday('now') - julianday(updated_at)` is EXACTLY the integer
  // cutoff, and with millisecond timestamps and a clock that advances
  // between the two `julianday` evaluations that equality is unreachable.
  // So the overclaiming name is gone, and this case instead pins the
  // boundary that IS observable and IS load-bearing: a row aged just
  // UNDER the cutoff is excluded and a row aged just OVER it is included.
  // That kills any off-by-one in the cutoff (verified: mutating the SQL to
  // `> {} - 1` or `> {} + 1` fails this case).
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  const auto      young = add_simple(conn, "young.txt", "young");
  const auto      old   = add_simple(conn, "old.txt", "old");
  REQUIRE(ann::resolve(conn, young).has_value());
  REQUIRE(ann::resolve(conn, old).has_value());

  // 29.5 days old vs 30.5 days old, against a 30-day cutoff.
  exec(conn, std::format("update annotations set updated_at = "
                         "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-29.5 days') where id = {}",
                         young));
  exec(conn, std::format("update annotations set updated_at = "
                         "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-30.5 days') where id = {}",
                         old));

  auto at_thirty = ann::sweep_candidates(conn, 30);
  REQUIRE(at_thirty.has_value());
  REQUIRE(at_thirty->size() == 1);
  CHECK((*at_thirty)[0] == old);

  // Widen the cutoff past both and neither qualifies; narrow it and both do.
  auto none = ann::sweep_candidates(conn, 31);
  REQUIRE(none.has_value());
  CHECK(none->empty());

  auto both = ann::sweep_candidates(conn, 29);
  REQUIRE(both.has_value());
  CHECK(both->size() == 2);
}

TEST_CASE("the sweep envelopes match the oracle byte for byte", "[annotation][parity]") {
  CHECK(ann::render_sweep_json(30, 0) == R"({"ok":true,"action":"sweep","since_days":30,"swept":0})");
  CHECK(ann::render_sweep_json(0, 7) == R"({"ok":true,"action":"sweep","since_days":0,"swept":7})");
  CHECK(ann::render_sweep_text(30, 0) == "sweep: archived 0 annotation(s) older than 30 day(s)");
  // Note the argument ORDER inversion between the JSON and text forms:
  // JSON is (since_days, swept), text prints (swept, since_days).
  CHECK(ann::render_sweep_text(7, 2) == "sweep: archived 2 annotation(s) older than 7 day(s)");
}

// ---------------------------------------------------------------------------
// annotate verify
// ---------------------------------------------------------------------------

TEST_CASE("the embedded SHA-256 matches the FIPS 180-4 vectors", "[annotation][sha256]") {
  // classify_anchor's verdict is only as trustworthy as this primitive,
  // and this tree vendors no crypto library, so it is pinned directly
  // against the published vectors -- plus the exact file the oracle probe
  // used.
  //
  // The oracle probe hashed a file holding "hello\n":
  //   shasum -a 256 h.txt
  //     -> 5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03
  // and the annotation carrying that value classified `fresh`.
  CHECK(ann::classify_anchor("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", std::string_view{""}) ==
        ann::verify_state::fresh);
  CHECK(ann::classify_anchor("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", std::string_view{"abc"}) ==
        ann::verify_state::fresh);
  CHECK(ann::classify_anchor("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                             std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}) ==
        ann::verify_state::fresh);
  CHECK(ann::classify_anchor("5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03", std::string_view{"hello\n"}) ==
        ann::verify_state::fresh);
  // A multi-block input (>55 bytes forces a second padding block).
  CHECK(ann::classify_anchor("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                             std::string_view{std::string(1000000, 'a')}) == ann::verify_state::fresh);
}

TEST_CASE("classify_anchor: unreadable is stale, no stored hash is fresh", "[annotation][parity]") {
  // Oracle: the `missing.txt` annotation classified `stale`; the three
  // `z.txt` annotations (no file on disk) also classified `stale`.
  CHECK(ann::classify_anchor("anything", std::nullopt) == ann::verify_state::stale);
  CHECK(ann::classify_anchor("", std::nullopt) == ann::verify_state::stale);
  // No stored hash means no drift SIGNAL, so a readable file is fresh --
  // it is NOT treated as drifted just because there is nothing to compare.
  CHECK(ann::classify_anchor("", std::string_view{"whatever"}) == ann::verify_state::fresh);
}

TEST_CASE("classify_anchor: a mismatched hash is drifted", "[annotation][parity]") {
  // Oracle: the annotation carrying `--text-hash 0000000000` on a readable
  // h.txt classified `drifted`.
  CHECK(ann::classify_anchor("0000000000", std::string_view{"hello\n"}) == ann::verify_state::drifted);
}

TEST_CASE("classify_anchor accepts a stored hash PREFIX", "[annotation][parity]") {
  // The Zig comparison truncates the COMPUTED digest to the stored value's
  // length before comparing, so a prefix matches. Deliberate, preserved.
  CHECK(ann::classify_anchor("5891b5b5", std::string_view{"hello\n"}) == ann::verify_state::fresh);
  CHECK(ann::classify_anchor("5891b5b6", std::string_view{"hello\n"}) == ann::verify_state::drifted);
  // And a stored value LONGER than the 64-hex digest can never match,
  // because the truncation caps at 64.
  CHECK(ann::classify_anchor("5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03EXTRA",
                             std::string_view{"hello\n"}) == ann::verify_state::drifted);
}

TEST_CASE("the verify envelopes match the oracle byte for byte", "[annotation][parity]") {
  const std::vector<ann::verify_row> rows{
      {.id = 1, .anchor_path = "z.txt", .state = ann::verify_state::stale},
      {.id = 4, .anchor_path = "h.txt", .state = ann::verify_state::fresh},
      {.id = 5, .anchor_path = "h.txt", .state = ann::verify_state::drifted},
  };
  CHECK(ann::render_verify_json(rows) == R"({"ok":true,"rows":[{"id":1,"anchor_path":"z.txt","state":"stale"},)"
                                         R"({"id":4,"anchor_path":"h.txt","state":"fresh"},)"
                                         R"({"id":5,"anchor_path":"h.txt","state":"drifted"}]})");
  CHECK(ann::render_verify_text(rows) == "annotation:1  z.txt  [stale]\n"
                                         "annotation:4  h.txt  [fresh]\n"
                                         "annotation:5  h.txt  [drifted]\n");

  CHECK(ann::render_verify_json({}) == R"({"ok":true,"rows":[]})");
  CHECK(ann::render_verify_text({}) == "(no active annotations)\n");
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

namespace {

/// The `annotate add --anchor-path a.txt --title A1 --body b1
/// --tags "x, y ,x"` row exactly as the oracle emitted it, reconstructed
/// as a value so the renderers can be compared byte for byte.
auto oracle_fixture() -> ann::annotation {
  return ann::annotation{
      .id          = 1,
      .scope_kind_ = ann::scope_kind::global,
      .scope_id    = std::nullopt,
      .anchor      = ann::anchor_fields{.path = "a.txt"},
      .title       = std::string{"A1"},
      .slug        = std::nullopt,
      .body        = "b1",
      .status_     = ann::status::active,
      .vendor      = "",
      .plan_id     = std::nullopt,
      .task_id     = std::nullopt,
      .tags        = {"x", "y"},
      .created_at  = "2026-08-23T01:01:48.528Z",
      .updated_at  = "2026-08-23T01:01:48.528Z",
  };
}

} // namespace

TEST_CASE("render_json is byte-identical to the oracle's --json object", "[annotation][parity]") {
  CHECK(ann::render_json(oracle_fixture()) ==
        R"({"id":1,"scope_kind":"global","scope_id":null,"anchor":{"path":"a.txt","line_start":null,)"
        R"("line_end":null,"commit_sha":"","text_hash":"","text":""},"title":"A1","slug":null,)"
        R"("body":"b1","status":"active","vendor":"","plan_id":null,"task_id":null,"tags":["x","y"],)"
        R"("created_at":"2026-08-23T01:01:48.528Z","updated_at":"2026-08-23T01:01:48.528Z"})");
}

TEST_CASE("render_json emits every optional as an explicit null", "[annotation][parity]") {
  ann::annotation bare{
      .id         = 3,
      .anchor     = ann::anchor_fields{.path = "b.txt", .line_start = 3, .line_end = 9},
      .title      = std::string{"A3"},
      .created_at = "2026-08-23T01:01:48.594Z",
      .updated_at = "2026-08-23T01:01:48.594Z",
  };
  // Captured from `annotate add --anchor-path b.txt --title A3
  // --line-start 3 --line-end 9 --json`.
  CHECK(ann::render_json(bare) == R"({"id":3,"scope_kind":"global","scope_id":null,"anchor":{"path":"b.txt","line_start":3,)"
                                  R"("line_end":9,"commit_sha":"","text_hash":"","text":""},"title":"A3","slug":null,)"
                                  R"("body":"","status":"active","vendor":"","plan_id":null,"task_id":null,"tags":[],)"
                                  R"("created_at":"2026-08-23T01:01:48.594Z","updated_at":"2026-08-23T01:01:48.594Z"})");
}

TEST_CASE("render_list_json wraps objects in a bare array", "[annotation][parity]") {
  CHECK(ann::render_list_json({}) == "[]");
  const auto one = oracle_fixture();
  CHECK(ann::render_list_json({one, one}) == std::format("[{},{}]", ann::render_json(one), ann::render_json(one)));
}

TEST_CASE("render_text is byte-identical to the oracle's show output", "[annotation][parity]") {
  // Captured from `annotate show 1` -- note that `slug`, `anchor line`,
  // `anchor sha`, `anchor hash`, `vendor`, `plan` and `task` are ABSENT,
  // not printed empty.
  CHECK(ann::render_text(oracle_fixture()) == "id:          1\n"
                                              "title:       A1\n"
                                              "status:      active\n"
                                              "scope:       global\n"
                                              "anchor path: a.txt\n"
                                              "tags:        x, y\n"
                                              "body:        b1\n"
                                              "created:     2026-08-23T01:01:48.528Z\n"
                                              "updated:     2026-08-23T01:01:48.528Z\n");
}

TEST_CASE("render_text prints every optional field when present", "[annotation][parity]") {
  ann::annotation full{
      .id          = 9,
      .scope_kind_ = ann::scope_kind::association,
      .scope_id    = 2,
      .anchor =
          ann::anchor_fields{.path = "src/x.zig", .line_start = 10, .line_end = 20, .commit_sha = "abc123", .text_hash = "hh"},
      .title      = std::string{"T"},
      .slug       = std::string{"s"},
      .body       = "B",
      .status_    = ann::status::resolved,
      .vendor     = "claude",
      .plan_id    = 4,
      .task_id    = 5,
      .tags       = {"a", "b"},
      .created_at = "C",
      .updated_at = "U",
  };
  CHECK(ann::render_text(full) == "id:          9\n"
                                  "title:       T\n"
                                  "slug:        s\n"
                                  "status:      resolved\n"
                                  "scope:       association:2\n"
                                  "anchor path: src/x.zig\n"
                                  "anchor line: 10-20\n"
                                  "anchor sha:  abc123\n"
                                  "anchor hash: hh\n"
                                  "vendor:      claude\n"
                                  "plan:        4\n"
                                  "task:        5\n"
                                  "tags:        a, b\n"
                                  "body:        B\n"
                                  "created:     C\n"
                                  "updated:     U\n");
}

TEST_CASE("render_text prints a lone line_start without a range", "[annotation][parity]") {
  ann::annotation one_line{
      .id         = 1,
      .anchor     = ann::anchor_fields{.path = "a.txt", .line_start = 7},
      .created_at = "C",
      .updated_at = "U",
  };
  const auto out = ann::render_text(one_line);
  CHECK(out.find("anchor line: 7\n") != std::string::npos);
  CHECK(out.find("anchor line: 7-") == std::string::npos);
}

TEST_CASE("render_list_text matches the oracle's column layout", "[annotation][parity]") {
  // Captured from `annotate list`:
  //     1  active      A1
  //     2  active      A2
  //     3  active      A3
  // -- id right-aligned in 5, two spaces, status left-aligned in 10, two
  // spaces, then the title.
  std::vector<ann::annotation> items;
  for (std::int64_t i = 1; i <= 3; ++i) {
    items.push_back(ann::annotation{.id = i, .title = std::format("A{}", i)});
  }
  CHECK(ann::render_list_text(items) == "    1  active      A1\n"
                                        "    2  active      A2\n"
                                        "    3  active      A3\n");
  CHECK(ann::render_list_text({}) == "(no annotations)\n");
}

TEST_CASE("render_list_text falls back to the anchor path when there is no title", "[annotation][parity]") {
  std::vector<ann::annotation> items{
      ann::annotation{.id = 12, .anchor = ann::anchor_fields{.path = "src/x.zig"}, .status_ = ann::status::dismissed},
  };
  CHECK(ann::render_list_text(items) == "   12  dismissed   src/x.zig\n");
}

TEST_CASE("the tag and remove envelopes match the oracle byte for byte", "[annotation][parity]") {
  CHECK(ann::render_tag_json(1, "zz", false) == R"({"ok":true,"id":1,"tag":"zz","action":"add"})");
  CHECK(ann::render_tag_json(1, "zz", true) == R"({"ok":true,"id":1,"tag":"zz","action":"remove"})");
  CHECK(ann::render_tag_text(1, "qq", false) == "annotation 1: added tag 'qq'");
  CHECK(ann::render_tag_text(1, "qq", true) == "annotation 1: removed tag 'qq'");
  CHECK(ann::render_remove_json(3) == R"({"ok":true,"id":3})");
  CHECK(ann::render_remove_text(1) == "annotation 1 removed");
}

TEST_CASE("render_json escapes the WHOLE control-byte table, not just the metacharacters", "[annotation]") {
  // `anchor.text` holds EXTRACTED SOURCE and `body` holds operator free text,
  // so every byte below 0x20 is genuinely reachable here -- 0x0C in particular
  // is a real character in source files that use form feeds as page breaks.
  //
  // ORACLE PROVENANCE. Captured from the live Zig binary against a scratch DB,
  // driven from python3 because the shell mangles 0x08/0x0C:
  //
  //   env PLANAR_DB=/tmp/fix.db PLANAR_CONFIG_PATH=/tmp/fix.toml planar \
  //     annotate add --anchor-path a.zig --line-start 1 --line-end 2 \
  //     --commit-sha deadbeef --text-hash h --text <PROBE> --body <PROBE> --json
  //
  // where <PROBE> is bytes 0x01..0x1F followed by ` "\/ ` and 0x7F. The oracle
  // answered with exactly the expectation below: SHORT forms for 0x08, 0x09,
  // 0x0A, 0x0C and 0x0D; LOWERCASE `\u00xx` for every other C0 byte; a BARE
  // `/`; and 0x7F passed through RAW. (0x00 is excluded -- argv cannot carry
  // it, so it is not oracle-observable through this leaf.)
  //
  // The previous version of this test exercised only quote, backslash and
  // newline under the same name. That is exactly how three local escapers in
  // this tree dropped the `\b` / `\f` short forms for the whole of M4
  // without a single test noticing.
  constexpr std::string_view probe{"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17"
                                   "\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20\"\\/\x20\x7f"};
  constexpr std::string_view escaped{
      R"(\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f \"\\/ )"};

  ann::annotation hostile{
      .id         = 1,
      .anchor     = ann::anchor_fields{.path = R"(a"b\c)", .text = std::string{probe}},
      .body       = std::string{probe},
      .created_at = "C",
      .updated_at = "U",
  };
  const auto out = ann::render_json(hostile);

  // The two metacharacters the old test covered, kept.
  CHECK(out.find(R"("path":"a\"b\\c")") != std::string::npos);
  // `text` and `body` are SEPARATE call sites inside render_json; either
  // could regress alone, so both are pinned against the same table.
  CHECK(out.find(std::format(R"("text":"{}")", escaped)) != std::string::npos);
  CHECK(out.find(std::format(R"("body":"{}")", escaped)) != std::string::npos);
  // And nothing below 0x20 survived RAW anywhere in the envelope.
  for (unsigned char c = 0x01; c < 0x20; ++c) {
    CHECK(out.find(static_cast<char>(c)) == std::string::npos);
  }
}

TEST_CASE("status and scope_kind text round-trip", "[annotation]") {
  for (const auto s : {ann::status::active, ann::status::resolved, ann::status::dismissed, ann::status::archived}) {
    auto parsed = ann::status_from_text(ann::status_to_text(s));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == s);
  }
  CHECK_FALSE(ann::status_from_text("open").has_value());

  for (const auto k : {ann::scope_kind::global, ann::scope_kind::repo, ann::scope_kind::association}) {
    auto parsed = ann::scope_kind_from_text(ann::scope_kind_to_text(k));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == k);
  }
  CHECK_FALSE(ann::scope_kind_from_text("workspace").has_value());
}

TEST_CASE("receipt retention threshold preserves replay across a reopened source", "[annotation][receipt][6684]") {
  // The 10,000-receipt limit is informational: no maintenance action may
  // evict a receipt and reopen the duplicate-create window. Seed the soft
  // cap directly so this regression stays fast while exercising a real
  // receipt-backed create and a fresh connection (the restart boundary).
  scratch_db_path scratch;
  std::string     source;
  {
    auto conn     = open_migrated(scratch);
    auto identity = ann::source_uuid(conn);
    REQUIRE(identity.has_value());
    source = *identity;
    exec(conn, std::format("with recursive n(x) as (select 1 union all select x + 1 from n where x < 10000) "
                           "insert into annotation_operation_receipts(operation_uuid, source_uuid, payload_digest, outcome) "
                           "select printf('retained-%05d', x), '{}', printf('digest-%05d', x), 'create' from n;",
                           source));

    exec(conn,
         "insert into plans (scope_kind, title, slug, status) values ('global', 'receipt target', 'receipt-target', 'draft')");
    ann::command_args command{.operation      = ann::command_kind::create,
                              .operation_uuid = "retain-and-replay",
                              .source_uuid    = source,
                              .target         = ann::entity_target{.kind = ann::target_kind::plan, .id = 1},
                              .body           = "durable"};
    auto              first = ann::execute_command(conn, command);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->replayed);
    CHECK(scalar_int(conn, "select count(*) from annotation_operation_receipts") == 10001);
    CHECK(scalar_int(conn, "select count(*) from annotations") == 1);
  }

  auto              reopened = open_migrated(scratch);
  ann::command_args replay{.operation      = ann::command_kind::create,
                           .operation_uuid = "retain-and-replay",
                           .source_uuid    = source,
                           .target         = ann::entity_target{.kind = ann::target_kind::plan, .id = 1},
                           .body           = "durable"};
  auto              result = ann::execute_command(reopened, replay);
  REQUIRE(result.has_value());
  CHECK(result->replayed);
  CHECK(scalar_int(reopened, "select count(*) from annotation_operation_receipts") == 10001);
  CHECK(scalar_int(reopened, "select count(*) from annotations") == 1);
}
