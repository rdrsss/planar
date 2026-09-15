// @file diff.t.cpp
// @brief Unit tests for `planar.engine.ingest.diff` (plan 996, task 6035).
//
// Exercised against a real, migrated on-disk SQLite database rather than a
// stubbed connection: the reconciliation rules ARE the SQL (which rows count
// as existing, which statuses are excluded), so a fake store would test the
// wrong half of the module.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.ingest.diff;
import planar.engine.ingest.parse;

namespace {

namespace diff  = planar::engine::ingest::diff;
namespace parse = planar::engine::ingest::parse;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Mirrors
/// scope_ref/scope_ref.t.cpp's identical helper — duplicated rather than
/// shared because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ingest_diff_test_{}_{}.db",
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
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

auto must_execute(planar::db::connection& conn, std::string_view sql) -> void {
  const auto result = conn.execute(sql);
  REQUIRE(result.has_value());
}

/// @brief Seeds a root anchor plan and returns its id.
auto seed_anchor(planar::db::connection& conn, std::string_view slug, std::string_view status) -> std::int64_t {
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status) "
                                 "values ('global', 'Anchor', '{}', '{}')",
                                 slug, status));
  auto stmt = conn.prepare("select id from plans where slug = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  const auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Builds a roadmap work item.
[[nodiscard]] auto item_of(std::string_view title, std::string_view slug, std::string_view source_text) -> parse::work_item {
  return {.title_       = std::string{title},
          .touches_     = {},
          .depends_     = {},
          .slug_        = std::string{slug},
          .source_text_ = std::string{source_text}};
}

} // namespace

TEST_CASE("is_non_trivial requires at least two bullets", "[ingest][diff]") {
  CHECK_FALSE(diff::is_non_trivial(""));
  CHECK_FALSE(diff::is_non_trivial("- only one"));
  CHECK(diff::is_non_trivial("- one\n- two"));
  CHECK(diff::is_non_trivial("* one\n* two\n* three"));
}

TEST_CASE("build_task_body treats the roadmap item as the acceptance criterion", "[ingest][diff]") {
  parse::work_item item{
      .title_ = "Add foo", .touches_ = {"repo-a", "repo-b"}, .depends_ = {}, .slug_ = "", .source_text_ = "- Add foo"};
  // No " is implemented and tested." suffix: routing rejects that phrase as a
  // placeholder, so appending it to real authored roadmap text made every
  // generated task fail readiness on content Planar itself wrote.
  constexpr std::string_view expected = R"(## Acceptance Criteria

- Add foo

## Repository Scope

- touches: repo-a
- touches: repo-b

## Required validation

- Run the authored test-spec scenarios covering this task.
)";
  CHECK(diff::build_task_body(item) == expected);
}

TEST_CASE("build_task_body omits the Repository Scope section without touches", "[ingest][diff]") {
  const parse::work_item item{.title_ = "Add bar", .touches_ = {}, .depends_ = {}, .slug_ = "", .source_text_ = "- Add bar"};
  CHECK(diff::build_task_body(item) == "## Acceptance Criteria\n\n- Add bar\n\n## Required validation\n\n"
                                       "- Run the authored test-spec scenarios covering this task.\n");
}

TEST_CASE("both shipped generated body shapes are recognised as generated", "[ingest][diff]") {
  const parse::work_item item{.title_ = "Add baz", .touches_ = {}, .depends_ = {}, .slug_ = "", .source_text_ = "- Add baz"};
  const auto             current = diff::build_task_body(item);
  const auto             legacy  = diff::build_legacy_task_body(item);
  REQUIRE(current != legacy);

  // If the legacy shape were not recognised, every task written before the
  // suffix was dropped would be mistaken for operator work and its generic
  // body frozen forever — the fix would look right on new tasks only.
  CHECK(diff::is_generated_task_body(current, item));
  CHECK(diff::is_generated_task_body(legacy, item));
  // Real operator content must survive re-ingest.
  CHECK_FALSE(diff::is_generated_task_body("## Acceptance Criteria\n\n- Streams reconnect within 2s of a drop\n", item));
}

TEST_CASE("build_task_next_action names the roadmap position, not a platitude", "[ingest][diff]") {
  const parse::work_item item{.title_ = "Add qux", .touches_ = {}, .depends_ = {}, .slug_ = "", .source_text_ = "- Add qux"};
  const auto             next_action = diff::build_task_next_action(item, 6, 1);
  CHECK(next_action == "Deliver roadmap milestone 6 item 1: Add qux");
  CHECK(next_action != diff::legacy_next_action);
}

TEST_CASE("build_scenario_body composes Verifies, Kind, Acceptance and prose", "[ingest][diff]") {
  const parse::scenario s{.title_      = "x",
                          .kind_       = "integration",
                          .acceptance_ = "exits 0",
                          .verifies_   = {{.kind_ = "task", .id_ = 0, .slug_ = "foo"}, {.kind_ = "task", .id_ = 5, .slug_ = ""}},
                          .body_       = "prose line"};
  constexpr std::string_view expected = R"(**Verifies:** task:foo, task:5
**Kind:** integration
**Acceptance:** exits 0

prose line)";
  CHECK(diff::build_scenario_body(s) == expected);
}

TEST_CASE("compute returns an empty diff for empty input", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "draft");

  const auto result = diff::compute(conn, anchor_id, {}, {}, {}, {});
  REQUIRE(result.has_value());
  CHECK(result->is_empty());
  CHECK(result->anchor_slug_ == "anchor");
  CHECK(result->current_status_ == "draft");
}

TEST_CASE("compute refuses an anchor that is not a root plan", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "draft");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'Child', 'child', 'active', {})",
                                 anchor_id));

  auto stmt = conn.prepare("select id from plans where slug = 'child'");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  const auto child_id = stmt->column_int64(0);

  // Pointing ingest at a child plan must be a clean refusal, not a diff
  // computed against the wrong subtree.
  const auto result = diff::compute(conn, child_id, {}, {}, {}, {});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == diff::diff_error::not_found);
}

TEST_CASE("compute proposes a child plan, its tasks, and their citations", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "draft");

  const std::vector<parse::milestone> milestones{
      {.name_       = "M1",
       .intent_     = "first",
       .work_items_ = {item_of("Add foo", "add-foo", "- Add foo [slug: add-foo]"), item_of("Add bar", "", "- Add bar")}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());

  REQUIRE(result->child_plans_.size() == 1);
  CHECK(result->child_plans_[0].op_ == diff::op::add);
  CHECK(result->child_plans_[0].title_ == "M1");
  REQUIRE(result->child_plans_[0].tasks_.size() == 2);
  CHECK(result->child_plans_[0].tasks_[0].title_ == "Add foo");
  CHECK(result->child_plans_[0].tasks_[0].slug_ == "add-foo");
  CHECK(result->child_plans_[0].tasks_[1].title_ == "Add bar");
  CHECK(result->child_plans_[0].tasks_[1].slug_.empty());

  // Locators are 1-based and carry the canonical folded bullet.
  REQUIRE(result->roadmap_citations_.size() == 2);
  CHECK(result->roadmap_citations_[0].source_locator_ == "roadmap#milestone:1/item:1");
  CHECK(result->roadmap_citations_[0].source_text_ == "- Add foo [slug: add-foo]");
  CHECK(result->roadmap_citations_[1].source_locator_ == "roadmap#milestone:1/item:2");
  CHECK(result->roadmap_citations_[1].source_text_ == "- Add bar");
}

TEST_CASE("compute matches an existing milestone by title, case-insensitively", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'M1', 'm1', 'active', {})",
                                 anchor_id));

  const std::vector<parse::milestone> milestones{{.name_ = "  m1  ", .intent_ = "", .work_items_ = {}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->child_plans_.size() == 1);
  CHECK(result->child_plans_[0].op_ == diff::op::update);
  CHECK(result->child_plans_[0].existing_id_ != 0);
  // Matched, so it is not also proposed for removal.
  CHECK(result->orphan_plans_.empty());
}

TEST_CASE("compute proposes a stored milestone absent from the roadmap for removal", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'Gone', 'gone', 'active', {})",
                                 anchor_id));

  const auto result = diff::compute(conn, anchor_id, {}, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->orphan_plans_.size() == 1);
  CHECK(result->orphan_plans_[0].op_ == diff::op::remove);
  CHECK(result->orphan_plans_[0].title_ == "Gone");
  CHECK(result->total_removals() == 1);
}

TEST_CASE("compute preserves an operator-enriched task body across re-ingest", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'M1', 'm1', 'active', {})",
                                 anchor_id));
  // The enriched body is real operator work. Destroying it on re-ingest is
  // what made enrichment impossible in the first place.
  must_execute(conn, "insert into tasks (scope_kind, plan_id, title, body, next_action, slug) "
                     "values ('global', 2, 'Add foo', "
                     "'## Acceptance Criteria\n\n- Streams reconnect within 2s\n', "
                     "'Read src/reconnect.cpp first.', 'add-foo')");
  must_execute(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('task', 1, 'plan', 2, 'derives-from')");

  const std::vector<parse::milestone> milestones{
      {.name_ = "M1", .intent_ = "", .work_items_ = {item_of("Add foo", "add-foo", "- Add foo [slug: add-foo]")}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->child_plans_.size() == 1);
  // Body unchanged AND slug already set: nothing to propose at all.
  CHECK(result->child_plans_[0].tasks_.empty());
}

TEST_CASE("compute back-fills a slug onto a stored task that has none", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'M1', 'm1', 'active', {})",
                                 anchor_id));
  must_execute(conn, "insert into tasks (scope_kind, plan_id, title, body, next_action) "
                     "values ('global', 2, 'Add foo', "
                     "'## Acceptance Criteria\n\n- Operator wrote this\n', 'Refined next action.')");
  must_execute(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('task', 1, 'plan', 2, 'derives-from')");

  const std::vector<parse::milestone> milestones{
      {.name_ = "M1", .intent_ = "", .work_items_ = {item_of("Add foo", "add-foo", "- Add foo [slug: add-foo]")}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->child_plans_.size() == 1);
  REQUIRE(result->child_plans_[0].tasks_.size() == 1);
  const auto& task = result->child_plans_[0].tasks_[0];
  CHECK(task.op_ == diff::op::update);
  CHECK(task.slug_ == "add-foo");
  // The enriched body is NOT replaced — an empty body means "leave it alone".
  CHECK(task.body_.empty());
  // Nor is the refined next action.
  CHECK(task.next_action_.empty());
}

TEST_CASE("compute replaces a still-legacy next action but not a refined one", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                                 "values ('global', 'M1', 'm1', 'active', {})",
                                 anchor_id));
  must_execute(conn, "insert into tasks (scope_kind, plan_id, title, body, next_action) "
                     "values ('global', 2, 'Add foo', "
                     "'## Acceptance Criteria\n\n- Add foo is implemented and tested.\n', "
                     "'Implement per acceptance criteria.')");
  must_execute(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('task', 1, 'plan', 2, 'derives-from')");

  const std::vector<parse::milestone> milestones{
      {.name_ = "M1", .intent_ = "", .work_items_ = {item_of("Add foo", "add-foo", "- Add foo")}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->child_plans_[0].tasks_.size() == 1);
  const auto& task = result->child_plans_[0].tasks_[0];
  // The legacy-suffixed body IS generated, so it upgrades to the current shape.
  CHECK(task.body_ == "## Acceptance Criteria\n\n- Add foo\n\n## Required validation\n\n"
                      "- Run the authored test-spec scenarios covering this task.\n");
  CHECK(task.next_action_ == "Deliver roadmap milestone 1 item 1: Add foo");
}

TEST_CASE("compute surfaces a globally-held slug as a collision, even on a done task", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "draft");
  // A task on an unrelated plan already holds the slug. The unique index is
  // status-agnostic, so a DONE holder still blocks the apply — and is exactly
  // the holder an operator's obvious search misses.
  must_execute(conn, "insert into plans (scope_kind, title, slug, status) "
                     "values ('global', 'Elsewhere', 'elsewhere', 'active')");
  must_execute(conn, "insert into tasks (scope_kind, plan_id, title, slug, status) "
                     "values ('global', 2, 'Older work', 'add-foo', 'done')");

  const std::vector<parse::milestone> milestones{
      {.name_ = "M1", .intent_ = "", .work_items_ = {item_of("Add foo", "add-foo", "- Add foo")}}};

  const auto result = diff::compute(conn, anchor_id, milestones, {}, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->slug_collisions_.size() == 1);
  CHECK(result->slug_collisions_[0].slug_ == "add-foo");
  CHECK(result->slug_collisions_[0].existing_task_id_ == 1);
  CHECK(result->slug_collisions_[0].existing_plan_id_ == 2);
}

TEST_CASE("compute proposes decisions and skips unchanged ones", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, "insert into decisions (scope_kind, title, body) values ('global', 'Kept', 'same body')");
  must_execute(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('decision', 1, 'plan', 1, 'derives-from')");

  const std::vector<parse::decision> decisions{{.title_ = "Kept", .body_ = "same body"},
                                               {.title_ = "Kept", .body_ = "changed body"},
                                               {.title_ = "Brand new", .body_ = "fresh"}};

  const auto result = diff::compute(conn, anchor_id, {}, decisions, {}, {});
  REQUIRE(result.has_value());
  REQUIRE(result->decisions_.size() == 2);
  CHECK(result->decisions_[0].op_ == diff::op::update);
  CHECK(result->decisions_[0].body_ == "changed body");
  CHECK(result->decisions_[1].op_ == diff::op::add);
  CHECK(result->decisions_[1].title_ == "Brand new");
}

TEST_CASE("compute flips a stored open question with a resolution and derives a decision", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");
  must_execute(conn, "insert into questions (scope_kind, title, body, status) "
                     "values ('global', 'Which store?', 'body', 'open')");

  const std::vector<parse::question> questions{{.title_ = "Which store?", .body_ = "body", .resolution_ = "SQLite"}};

  const auto result = diff::compute(conn, anchor_id, {}, {}, questions, {});
  REQUIRE(result.has_value());
  REQUIRE(result->updated_question_status_.size() == 1);
  CHECK(result->updated_question_status_[0].old_status_ == "open");
  CHECK(result->updated_question_status_[0].new_status_ == "answered");
  CHECK(result->updated_question_status_[0].answer_ == "SQLite");
  // The resolution also becomes a decision, since none by that title exists.
  REQUIRE(result->decisions_.size() == 1);
  CHECK(result->decisions_[0].title_ == "Which store?");
  CHECK(result->decisions_[0].body_ == "SQLite");
  CHECK(result->new_questions_.empty());
}

TEST_CASE("compute proposes an unseen question as new", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");

  const std::vector<parse::question> questions{{.title_ = "Open one", .body_ = "b", .resolution_ = ""}};

  const auto result = diff::compute(conn, anchor_id, {}, {}, questions, {});
  REQUIRE(result.has_value());
  REQUIRE(result->new_questions_.size() == 1);
  CHECK(result->new_questions_[0].title_ == "Open one");
  CHECK(result->decisions_.empty());
  CHECK(result->total_additions() == 1);
}

TEST_CASE("compute proposes scenarios and skips ones whose body is unchanged", "[ingest][diff]") {
  const scratch_db_path scratch;
  auto                  conn      = open_migrated(scratch);
  const auto            anchor_id = seed_anchor(conn, "anchor", "active");

  const parse::scenario stored_shape{.title_ = "Kept", .kind_ = "unit", .acceptance_ = "passes", .verifies_ = {}, .body_ = ""};
  const auto            stored_body = diff::build_scenario_body(stored_shape);
  must_execute(conn, std::format("insert into test_scenarios (scope_kind, title, body) "
                                 "values ('global', 'Kept', '{}')",
                                 stored_body));
  must_execute(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                     "values ('test_scenario', 1, 'plan', 1, 'derives-from')");

  const std::vector<parse::scenario> scenarios{
      stored_shape, {.title_ = "Fresh", .kind_ = "integration", .acceptance_ = "exit 0", .verifies_ = {}, .body_ = ""}};

  const auto result = diff::compute(conn, anchor_id, {}, {}, {}, scenarios);
  REQUIRE(result.has_value());
  REQUIRE(result->scenarios_.size() == 1);
  CHECK(result->scenarios_[0].op_ == diff::op::add);
  CHECK(result->scenarios_[0].title_ == "Fresh");
}
