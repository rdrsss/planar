// @file question.t.cpp
// @brief Unit tests for `planar.engine.planning.question` (plan 996
// roadmap M12 item 4, task 6188).
//
// ## What these tests assert, and why they are shaped this way
//
// Ten wiring cycles into this milestone, every one of them has surfaced a
// latent engine bug that was unit-tested and green until a caller existed:
// a `list` with no status predicate, a renderer that emitted invalid JSON,
// a `--scope` that was accepted and ignored on a destructive sweep. The
// common shape is a test that asserts a COUNT, or asserts against the
// engine's own read path, and therefore cannot see the engine and the test
// agreeing on the same wrong thing.
//
// So, three rules here:
//
//   1. **Row assertions go through raw SQL**, not through `show_question`.
//      A test that reads back what it wrote with the same decoder cannot
//      catch a decoder that collapses SQL NULL onto the empty string.
//      `sql_text_or_null` renders NULL as the sentinel `<NULL>` so the two
//      are distinguishable at the assertion.
//   2. **Every filter is proven to EXCLUDE**, by asserting the
//      out-of-scope row SURVIVES in the table afterwards and by asserting
//      the returned ID SET rather than its size. A count assertion passes
//      for an inert filter that happens to return the right number of
//      wrong rows.
//   3. **The `--touches` branch asymmetry gets a THREE-WAY
//      discrimination**, not a pair of cases. The fixture is built so that
//      an inert scope filter returns the SAME non-empty set three times and
//      a uniformly-applied one returns EMPTY twice; only the real branch
//      asymmetry yields three DIFFERENT non-empty answers.
//
// One class of defect a raw-SQL row snapshot still cannot see: an engine
// READ that collapses NULL onto a default before the value ever reaches
// SQL. For that class the assertion is on the RENDERED form, where `null`
// and `""` are different bytes — see the render cases at the bottom.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.question;

namespace {

using planar::engine::planning::answer_question;
using planar::engine::planning::create_question;
using planar::engine::planning::list_questions;
using planar::engine::planning::list_questions_touching;
using planar::engine::planning::question;
using planar::engine::planning::question_create_args;
using planar::engine::planning::question_error;
using planar::engine::planning::question_list_filter;
using planar::engine::planning::question_scope_kind;
using planar::engine::planning::question_status;
using planar::engine::planning::render_json;
using planar::engine::planning::render_list_json;
using planar::engine::planning::render_list_text;
using planar::engine::planning::render_text;
using planar::engine::planning::show_question;
using planar::engine::planning::wontfix_question;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_question_test_{}_{}.db",
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

/// @brief Run a single-column, single-row query and return the value with
/// SQL NULL rendered as the literal `<NULL>`.
///
/// The sentinel is the whole point: `''` and NULL are different rows and a
/// test that cannot tell them apart cannot catch a bind that turned an
/// absent optional into an empty string.
auto sql_text_or_null(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return "<NO ROW>";
  }
  if (stmt->is_null(0)) {
    return "<NULL>";
  }
  return stmt->column_text(0);
}

auto sql_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

void exec(planar::db::connection& conn, std::string_view sql) {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief The ids of a result set, in the order the engine returned them.
auto ids_of(const std::vector<question>& rows) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  out.reserve(rows.size());
  for (const auto& q : rows) {
    out.push_back(q.id);
  }
  return out;
}

auto list_ids(planar::db::connection& conn, const question_list_filter& filter) -> std::vector<std::int64_t> {
  auto rows = list_questions(conn, filter);
  REQUIRE(rows.has_value());
  return ids_of(*rows);
}

auto touching_ids(planar::db::connection& conn, std::int64_t repo_id, const question_list_filter& filter)
    -> std::vector<std::int64_t> {
  auto rows = list_questions_touching(conn, repo_id, filter);
  REQUIRE(rows.has_value());
  return ids_of(*rows);
}

/// @brief Register an association and return its row id.
auto add_assoc(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn, std::format("insert into associations (slug, name, kind) values ('{}', '{}', 'org')", slug, slug));
  return sql_int(conn, std::format("select id from associations where slug = '{}'", slug));
}

/// @brief Register a project and return its row id.
auto add_repo(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn, std::format("insert into projects (slug, name, root_path) values ('{}', '{}', '/work/{}')", slug, slug, slug));
  return sql_int(conn, std::format("select id from projects where slug = '{}'", slug));
}

/// @brief Insert a plan directly (this module never creates one) and
/// return its row id.
auto add_plan(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn,
       std::format("insert into plans (scope_kind, title, slug, status) values ('global', '{}', '{}', 'draft')", slug, slug));
  return sql_int(conn, std::format("select id from plans where slug = '{}'", slug));
}

} // namespace

// --- CRUD ----------------------------------------------------------------

TEST_CASE("create writes the row the CALLER asked for, read back by raw SQL", "[question][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto q = create_question(conn, question_create_args{.title = "why?", .body = "because"});
  REQUIRE(q.has_value());
  CHECK(q->status == question_status::open);
  CHECK(q->scope_kind == question_scope_kind::global);

  // Row assertions through raw SQL, never through show_question — see this
  // file's header.
  auto col = [&](std::string_view name) {
    return sql_text_or_null(conn, std::format("select {} from questions where id = {}", name, q->id));
  };
  CHECK(col("title") == "why?");
  CHECK(col("body") == "because");
  CHECK(col("status") == "open");
  CHECK(col("scope_kind") == "global");
  // The three columns that must be SQL NULL, not "". A bind that turned an
  // absent optional into an empty string passes a `== ""` assertion and
  // fails this one.
  CHECK(col("scope_id") == "<NULL>");
  CHECK(col("answer_body") == "<NULL>");
  CHECK(col("answered_at") == "<NULL>");
}

TEST_CASE("create with an ABSENT body writes SQL NULL, not the empty string", "[question][crud][null]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto absent = create_question(conn, question_create_args{.title = "no body"});
  REQUIRE(absent.has_value());
  auto empty = create_question(conn, question_create_args{.title = "empty body", .body = std::string{}});
  REQUIRE(empty.has_value());

  // The discrimination the whole `<NULL>` sentinel exists for: these two
  // rows are DIFFERENT, and an engine that folded one onto the other would
  // pass every count- and title-based assertion in this file.
  CHECK(sql_text_or_null(conn, std::format("select body from questions where id = {}", absent->id)) == "<NULL>");
  CHECK(sql_text_or_null(conn, std::format("select body from questions where id = {}", empty->id)) == "");
  CHECK_FALSE(absent->body.has_value());
  REQUIRE(empty->body.has_value());
  CHECK(empty->body->empty());
}

TEST_CASE("create resolves each scope grammar onto the stored (kind, id) pair", "[question][crud][scope]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = add_assoc(conn, "acme");
  const auto      repo_id  = add_repo(conn, "widgets");

  auto global_q = create_question(conn, question_create_args{.title = "g", .scope = "global"});
  REQUIRE(global_q.has_value());
  CHECK(global_q->scope_kind == question_scope_kind::global);
  CHECK_FALSE(global_q->scope_id.has_value());
  CHECK(sql_text_or_null(conn, std::format("select scope_id from questions where id = {}", global_q->id)) == "<NULL>");

  auto assoc_q = create_question(conn, question_create_args{.title = "a", .scope = "acme"});
  REQUIRE(assoc_q.has_value());
  CHECK(assoc_q->scope_kind == question_scope_kind::association);
  REQUIRE(assoc_q->scope_id.has_value());
  CHECK(*assoc_q->scope_id == assoc_id);

  auto repo_q = create_question(conn, question_create_args{.title = "r", .scope = "repo:widgets"});
  REQUIRE(repo_q.has_value());
  CHECK(repo_q->scope_kind == question_scope_kind::repo);
  REQUIRE(repo_q->scope_id.has_value());
  CHECK(*repo_q->scope_id == repo_id);
}

TEST_CASE("create with an unresolvable scope refuses and writes NOTHING", "[question][crud][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto r = create_question(conn, question_create_args{.title = "x", .scope = "no-such-slug"});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::slug_not_found);
  // A refusal that had already inserted would be the silent half-write this
  // milestone keeps closing.
  CHECK(sql_int(conn, "select count(*) from questions") == 0);
  CHECK(sql_int(conn, "select count(*) from audit_log") == 0);
}

TEST_CASE("create --plan links the question and writes exactly ONE audit row", "[question][crud][plan]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = add_plan(conn, "the-plan");

  auto q = create_question(conn, question_create_args{.title = "linked", .plan_id = plan_id});
  REQUIRE(q.has_value());

  CHECK(sql_int(conn, std::format("select count(*) from entity_links where from_kind = 'question' and from_id = {} "
                                  "and to_kind = 'plan' and to_id = {} and relationship = 'derives-from'",
                                  q->id, plan_id)) == 1);
  // ORACLE: exactly one row, verb `create`. The edge write does NOT get a
  // `link` row of its own.
  CHECK(sql_int(conn, "select count(*) from audit_log") == 1);
  CHECK(sql_text_or_null(conn, "select verb from audit_log") == "create");
  CHECK(sql_text_or_null(conn, "select summary from audit_log") == "create question 'linked'");
  // `actor` and `scope` are always NULL on the CLI path.
  CHECK(sql_text_or_null(conn, "select actor from audit_log") == "<NULL>");
  CHECK(sql_text_or_null(conn, "select scope from audit_log") == "<NULL>");
}

TEST_CASE("create --plan naming a MISSING plan refuses before inserting anything", "[question][crud][plan]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto r = create_question(conn, question_create_args{.title = "dangling", .plan_id = 999});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::not_found);
  CHECK(sql_int(conn, "select count(*) from questions") == 0);
  CHECK(sql_int(conn, "select count(*) from entity_links") == 0);
  CHECK(sql_int(conn, "select count(*) from audit_log") == 0);
}

TEST_CASE("show: absent id returns not_found", "[question][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = show_question(conn, 999999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::not_found);
}

// --- Transitions ---------------------------------------------------------

TEST_CASE("answer writes status, answer_body and answered_at in ONE statement", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            q    = create_question(conn, question_create_args{.title = "do or do not"});
  REQUIRE(q.has_value());

  auto answered = answer_question(conn, q->id, "do");
  REQUIRE(answered.has_value());
  CHECK(answered->status == question_status::answered);

  const auto where = std::format("from questions where id = {}", q->id);
  CHECK(sql_text_or_null(conn, std::format("select status {}", where)) == "answered");
  CHECK(sql_text_or_null(conn, std::format("select answer_body {}", where)) == "do");
  // The schema's row CHECK requires answered_at non-null alongside
  // status='answered'; assert the ROW rather than trusting the CHECK, since
  // a build with foreign_keys/CHECK enforcement off would let a half-write
  // through silently.
  CHECK(sql_text_or_null(conn, std::format("select answered_at {}", where)) != "<NULL>");
  CHECK(sql_int(conn, std::format("select count(*) from audit_log where verb = 'status_change' and entity_kind = 'question' "
                                  "and entity_id = {} and summary = 'answer: do'",
                                  q->id)) == 1);
}

TEST_CASE("answer with an EMPTY string refuses and leaves the row open", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            q    = create_question(conn, question_create_args{.title = "?"});
  REQUIRE(q.has_value());

  auto r = answer_question(conn, q->id, "");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::answer_required);
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", q->id)) == "open");
  CHECK(sql_text_or_null(conn, std::format("select answer_body from questions where id = {}", q->id)) == "<NULL>");
  // A refused mutation writes NO audit row — only the `create` one exists.
  CHECK(sql_int(conn, "select count(*) from audit_log where verb = 'status_change'") == 0);
}

TEST_CASE("answer on a MISSING id refuses without inventing a row", "[question][transition][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = answer_question(conn, 4242, "hello");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::not_found);
  CHECK(sql_int(conn, "select count(*) from questions") == 0);
  CHECK(sql_int(conn, "select count(*) from audit_log") == 0);
}

TEST_CASE("RE-answering an already-answered question overwrites both columns", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            q    = create_question(conn, question_create_args{.title = "again?"});
  REQUIRE(q.has_value());

  REQUIRE(answer_question(conn, q->id, "first").has_value());
  const auto first_at = sql_text_or_null(conn, std::format("select answered_at from questions where id = {}", q->id));

  // NOT a no-op and NOT a refusal: `check_transition` short-circuits on
  // `from == to` before the question matrix is consulted. Oracle-confirmed
  // by running `question answer` twice on the same row.
  auto second = answer_question(conn, q->id, "second");
  REQUIRE(second.has_value());
  CHECK(sql_text_or_null(conn, std::format("select answer_body from questions where id = {}", q->id)) == "second");
  CHECK(first_at != "<NULL>");
  CHECK(sql_int(conn, "select count(*) from audit_log where summary = 'answer: second'") == 1);
}

TEST_CASE("wontfix records the reason in the AUDIT row, not on the question", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            q    = create_question(conn, question_create_args{.title = "deprecated"});
  REQUIRE(q.has_value());

  auto wf = wontfix_question(conn, q->id, std::optional<std::string_view>{"answered elsewhere"});
  REQUIRE(wf.has_value());
  CHECK(wf->status == question_status::wontfix);
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", q->id)) == "wontfix");
  // `questions` has NO reason column, so the reason is recoverable only
  // from the audit trail. Asserting the row proves it actually landed
  // somewhere rather than being formatted and dropped.
  CHECK(sql_text_or_null(conn, "select summary from audit_log where verb = 'status_change'") == "wontfix: answered elsewhere");
  // answer_body stays NULL — wontfix must not borrow the answer columns.
  CHECK(sql_text_or_null(conn, std::format("select answer_body from questions where id = {}", q->id)) == "<NULL>");
}

TEST_CASE("wontfix with NO reason writes the bare word, not a trailing separator", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            q    = create_question(conn, question_create_args{.title = "meh"});
  REQUIRE(q.has_value());

  REQUIRE(wontfix_question(conn, q->id, std::nullopt).has_value());
  // Exactly `wontfix`, NOT `wontfix: ` — the trailing-separator shape that
  // has written a stray `_` elsewhere in this port.
  CHECK(sql_text_or_null(conn, "select summary from audit_log where verb = 'status_change'") == "wontfix");
}

TEST_CASE("both terminal states are TERMINAL: every cross edge refuses", "[question][transition]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto answered = create_question(conn, question_create_args{.title = "answered one"});
  REQUIRE(answered.has_value());
  REQUIRE(answer_question(conn, answered->id, "yes").has_value());
  auto refused_wf = wontfix_question(conn, answered->id, std::nullopt);
  REQUIRE_FALSE(refused_wf.has_value());
  CHECK(refused_wf.error() == question_error::illegal_transition);
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", answered->id)) == "answered");

  auto wf = create_question(conn, question_create_args{.title = "wontfix one"});
  REQUIRE(wf.has_value());
  REQUIRE(wontfix_question(conn, wf->id, std::nullopt).has_value());
  auto refused_ans = answer_question(conn, wf->id, "too late");
  REQUIRE_FALSE(refused_ans.has_value());
  CHECK(refused_ans.error() == question_error::illegal_transition);
  // The refused answer must not have written its columns before the check.
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", wf->id)) == "wontfix");
  CHECK(sql_text_or_null(conn, std::format("select answer_body from questions where id = {}", wf->id)) == "<NULL>");
  // Two creates + one answer + one wontfix = four rows; neither refusal
  // added a fifth.
  CHECK(sql_int(conn, "select count(*) from audit_log") == 4);
}

// --- list: every filter is proven to EXCLUDE -----------------------------

TEST_CASE("list's EMPTY status filter means `open`, not `everything`", "[question][list][filter]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto still_open = create_question(conn, question_create_args{.title = "open one"});
  REQUIRE(still_open.has_value());
  auto to_answer = create_question(conn, question_create_args{.title = "answered one"});
  REQUIRE(to_answer.has_value());
  REQUIRE(answer_question(conn, to_answer->id, "yes").has_value());
  auto to_wontfix = create_question(conn, question_create_args{.title = "wontfix one"});
  REQUIRE(to_wontfix.has_value());
  REQUIRE(wontfix_question(conn, to_wontfix->id, std::nullopt).has_value());

  // The defect this pins: reading "empty statuses" as "no predicate" would
  // return all three. Asserting the ID SET rather than the size is what
  // makes that visible — a size-1 assertion would also pass for a filter
  // that returned the WRONG single row.
  CHECK(list_ids(conn, question_list_filter{}) == std::vector<std::int64_t>{still_open->id});

  // ...and the excluded rows SURVIVE. A `list` that had deleted or mutated
  // them would satisfy the assertion above just as well.
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", to_answer->id)) == "answered");
  CHECK(sql_text_or_null(conn, std::format("select status from questions where id = {}", to_wontfix->id)) == "wontfix");
  CHECK(sql_int(conn, "select count(*) from questions") == 3);

  // Naming the statuses explicitly reaches them, which proves the rows were
  // EXCLUDED by the predicate rather than absent from the table.
  CHECK(list_ids(conn, question_list_filter{.statuses = {question_status::answered}}) ==
        std::vector<std::int64_t>{to_answer->id});
  CHECK(list_ids(conn, question_list_filter{.statuses = {question_status::open, question_status::answered,
                                                         question_status::wontfix}}) ==
        std::vector<std::int64_t>{still_open->id, to_answer->id, to_wontfix->id});
}

TEST_CASE("list's scope filter EXCLUDES, and the excluded rows survive", "[question][list][filter][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_assoc(conn, "acme");
  add_repo(conn, "widgets");

  auto g = create_question(conn, question_create_args{.title = "global q", .scope = "global"});
  REQUIRE(g.has_value());
  auto a = create_question(conn, question_create_args{.title = "assoc q", .scope = "acme"});
  REQUIRE(a.has_value());
  auto r = create_question(conn, question_create_args{.title = "repo q", .scope = "repo:widgets"});
  REQUIRE(r.has_value());

  CHECK(list_ids(conn, question_list_filter{.scope = "acme"}) == std::vector<std::int64_t>{a->id});
  CHECK(list_ids(conn, question_list_filter{.scope = "global"}) == std::vector<std::int64_t>{g->id});
  CHECK(list_ids(conn, question_list_filter{.scope = "repo:widgets"}) == std::vector<std::int64_t>{r->id});
  // `scope` and `scopes` OR into ONE disjunction, in that order.
  CHECK(list_ids(conn, question_list_filter{.scope = "global", .scopes = {"acme"}}) == std::vector<std::int64_t>{g->id, a->id});
  // NO scope member at all applies NO predicate — the arm that must not be
  // confused with the empty-STATUS arm above, which does apply one.
  CHECK(list_ids(conn, question_list_filter{}) == std::vector<std::int64_t>{g->id, a->id, r->id});

  CHECK(sql_int(conn, "select count(*) from questions") == 3);
}

TEST_CASE("list refuses when ANY scope member is unresolvable, rather than dropping it", "[question][list][filter][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_assoc(conn, "acme");
  REQUIRE(create_question(conn, question_create_args{.title = "assoc q", .scope = "acme"}).has_value());

  // Skipping the bad member would return a SHORT list that looks complete —
  // exit 0, plausible rows, wrong rows.
  auto r = list_questions(conn, question_list_filter{.scope = "acme", .scopes = {"nope"}});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == question_error::slug_not_found);
}

TEST_CASE("list's plan filter EXCLUDES unlinked questions and the WRONG plan's", "[question][list][filter][plan]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  const auto      plan_a = add_plan(conn, "plan-a");
  const auto      plan_b = add_plan(conn, "plan-b");

  auto linked_a = create_question(conn, question_create_args{.title = "on a", .plan_id = plan_a});
  REQUIRE(linked_a.has_value());
  auto linked_b = create_question(conn, question_create_args{.title = "on b", .plan_id = plan_b});
  REQUIRE(linked_b.has_value());
  auto unlinked = create_question(conn, question_create_args{.title = "on nothing"});
  REQUIRE(unlinked.has_value());

  CHECK(list_ids(conn, question_list_filter{.plan_id = plan_a}) == std::vector<std::int64_t>{linked_a->id});
  CHECK(list_ids(conn, question_list_filter{.plan_id = plan_b}) == std::vector<std::int64_t>{linked_b->id});
  // An inert filter would return all three here; a filter that matched on
  // `to_id` alone without the relationship would too, once another edge
  // kind existed.
  CHECK(list_ids(conn, question_list_filter{}) == std::vector<std::int64_t>{linked_a->id, linked_b->id, unlinked->id});
  CHECK(sql_int(conn, "select count(*) from questions") == 3);

  // The relationship IS part of the predicate: an edge of a DIFFERENT
  // relationship to the same plan must not satisfy it. `cites` is chosen
  // because it is in `entity_links`' CHECK set — an unaccepted value would
  // make this insert fail and the case pass vacuously.
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('question', {}, 'plan', {}, 'cites')",
                         unlinked->id, plan_a));
  CHECK(list_ids(conn, question_list_filter{.plan_id = plan_a}) == std::vector<std::int64_t>{linked_a->id});
}

// --- list_questions_touching: the three-way discrimination ----------------

TEST_CASE("touches: the direct branch is switched OFF by scope, not narrowed", "[question][touches][filter]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      repo_id = add_repo(conn, "widgets");
  const auto      other   = add_repo(conn, "gadgets");
  add_assoc(conn, "acme");

  // The fixture is built so THREE different scope filters must give THREE
  // different NON-EMPTY answers. An inert filter gives the same set every
  // time; a uniformly-applied one empties the middle two.
  auto direct = create_question(conn, question_create_args{.title = "direct", .scope = "repo:widgets"});
  REQUIRE(direct.has_value());
  auto global_edge = create_question(conn, question_create_args{.title = "global edge", .scope = "global"});
  REQUIRE(global_edge.has_value());
  auto assoc_edge = create_question(conn, question_create_args{.title = "assoc edge", .scope = "acme"});
  REQUIRE(assoc_edge.has_value());
  // Two rows that must be EXCLUDED by every case below and must SURVIVE:
  // one scoped to a different repo, one with an edge to a different repo.
  auto other_repo = create_question(conn, question_create_args{.title = "other repo", .scope = "repo:gadgets"});
  REQUIRE(other_repo.has_value());
  auto other_edge = create_question(conn, question_create_args{.title = "other edge", .scope = "global"});
  REQUIRE(other_edge.has_value());

  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
                         "('question', {}, 'repo', {}, 'touches'), ('question', {}, 'repo', {}, 'touches'), "
                         "('question', {}, 'repo', {}, 'touches')",
                         global_edge->id, repo_id, assoc_edge->id, repo_id, other_edge->id, other));

  // 1. NO scope set: the direct branch is ON and the touches branch is
  //    unfiltered, so both contribute.
  CHECK(touching_ids(conn, repo_id, question_list_filter{}) ==
        std::vector<std::int64_t>{direct->id, global_edge->id, assoc_edge->id});
  // 2. `global` alone: direct branch OFF entirely (it holds no global row
  //    to narrow), touches branch keeps only the global one.
  CHECK(touching_ids(conn, repo_id, question_list_filter{.scope = "global"}) == std::vector<std::int64_t>{global_edge->id});
  // 3. `acme` alone: same shape, different single row. Three DIFFERENT
  //    non-empty answers — which is what an inert filter cannot produce.
  CHECK(touching_ids(conn, repo_id, question_list_filter{.scope = "acme"}) == std::vector<std::int64_t>{assoc_edge->id});
  // 4. THIS repo by slug: direct branch back ON, touches branch now
  //    admits nothing (no touching row is repo-scoped to widgets).
  CHECK(touching_ids(conn, repo_id, question_list_filter{.scope = "repo:widgets"}) == std::vector<std::int64_t>{direct->id});
  // 5. A DIFFERENT repo: direct branch OFF (the ref does not name repo_id),
  //    touches branch admits nothing. The one legitimately EMPTY answer,
  //    and it is empty for a reason the other four rule out.
  CHECK(touching_ids(conn, repo_id, question_list_filter{.scope = "repo:gadgets"}).empty());

  // Every excluded row SURVIVES — the listing filtered, it did not delete.
  CHECK(sql_int(conn, "select count(*) from questions") == 5);
  CHECK(sql_text_or_null(conn, std::format("select title from questions where id = {}", other_repo->id)) == "other repo");
  CHECK(sql_text_or_null(conn, std::format("select title from questions where id = {}", other_edge->id)) == "other edge");
}

TEST_CASE("touches: the status default is applied in EACH branch independently", "[question][touches][filter]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      repo_id = add_repo(conn, "widgets");

  auto direct_open = create_question(conn, question_create_args{.title = "direct open", .scope = "repo:widgets"});
  REQUIRE(direct_open.has_value());
  auto direct_done = create_question(conn, question_create_args{.title = "direct answered", .scope = "repo:widgets"});
  REQUIRE(direct_done.has_value());
  REQUIRE(answer_question(conn, direct_done->id, "yes").has_value());
  auto edge_open = create_question(conn, question_create_args{.title = "edge open", .scope = "global"});
  REQUIRE(edge_open.has_value());
  auto edge_done = create_question(conn, question_create_args{.title = "edge answered", .scope = "global"});
  REQUIRE(edge_done.has_value());
  REQUIRE(answer_question(conn, edge_done->id, "yes").has_value());
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
                         "('question', {}, 'repo', {}, 'touches'), ('question', {}, 'repo', {}, 'touches')",
                         edge_open->id, repo_id, edge_done->id, repo_id));

  // Losing the default on EITHER arm leaks that arm's terminal rows. Both
  // arms are represented in this fixture, so a one-sided omission shows up.
  CHECK(touching_ids(conn, repo_id, question_list_filter{}) == std::vector<std::int64_t>{direct_open->id, edge_open->id});
  CHECK(touching_ids(conn, repo_id, question_list_filter{.statuses = {question_status::answered}}) ==
        std::vector<std::int64_t>{direct_done->id, edge_done->id});
  CHECK(sql_int(conn, "select count(*) from questions") == 4);
}

TEST_CASE("touches: the plan filter is IGNORED, matching the oracle", "[question][touches][filter]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      repo_id = add_repo(conn, "widgets");
  const auto      plan_id = add_plan(conn, "the-plan");

  auto linked = create_question(conn, question_create_args{.title = "linked", .scope = "repo:widgets", .plan_id = plan_id});
  REQUIRE(linked.has_value());
  auto unlinked = create_question(conn, question_create_args{.title = "unlinked", .scope = "repo:widgets"});
  REQUIRE(unlinked.has_value());

  // `list_questions` HONOURS `plan_id`; `list_questions_touching` does not.
  // The asymmetry is the oracle's (`listTouching` never mentions the
  // `derives-from` subquery) and is pinned so a later "consistency" fix has
  // to argue with a test rather than pass silently.
  CHECK(list_ids(conn, question_list_filter{.plan_id = plan_id}) == std::vector<std::int64_t>{linked->id});
  CHECK(touching_ids(conn, repo_id, question_list_filter{.plan_id = plan_id}) ==
        std::vector<std::int64_t>{linked->id, unlinked->id});
}

// --- Renderers -----------------------------------------------------------
//
// These assert the RENDERED BYTES, which is the one place a NULL that the
// engine collapsed onto a default is visible: a raw-SQL row snapshot reads
// the column, not the decode.

TEST_CASE("render_text emits the oracle's exact block, conditionals included", "[question][render]") {
  const question minimal{
      .id         = 1,
      .scope_kind = question_scope_kind::global,
      .title      = "first question",
      .status     = question_status::open,
      .created_at = "2026-08-26T09:31:08.323Z",
      .updated_at = "2026-08-26T09:31:08.323Z",
  };
  // Oracle-captured verbatim. Values start at column 12 — one wider than
  // `plan`'s block.
  CHECK(render_text(minimal) == "id:        1\n"
                                "title:     first question\n"
                                "status:    open\n"
                                "scope:     global\n"
                                "created:   2026-08-26T09:31:08.323Z\n"
                                "updated:   2026-08-26T09:31:08.323Z\n");

  const question full{
      .id          = 2,
      .scope_kind  = question_scope_kind::repo,
      .scope_id    = 1,
      .title       = "scoped q",
      .body        = "some body",
      .status      = question_status::answered,
      .answer_body = "the answer",
      .answered_at = "2026-08-26T09:31:24.414Z",
      .created_at  = "2026-08-26T09:31:24.216Z",
      .updated_at  = "2026-08-26T09:31:24.414Z",
  };
  // `scope:` gains `:<id>`; the three conditional lines appear in
  // body/answer/answered order between `scope:` and `created:`.
  CHECK(render_text(full) == "id:        2\n"
                             "title:     scoped q\n"
                             "status:    answered\n"
                             "scope:     repo:1\n"
                             "body:      some body\n"
                             "answer:    the answer\n"
                             "answered:  2026-08-26T09:31:24.414Z\n"
                             "created:   2026-08-26T09:31:24.216Z\n"
                             "updated:   2026-08-26T09:31:24.414Z\n");
}

TEST_CASE("render_json distinguishes SQL NULL from the empty string", "[question][render][null]") {
  question q{
      .id         = 1,
      .scope_kind = question_scope_kind::global,
      .title      = "first question",
      .status     = question_status::open,
      .created_at = "2026-08-26T09:31:08.323Z",
      .updated_at = "2026-08-26T09:31:08.323Z",
  };
  // Oracle-captured. No trailing newline — a fragment the caller terminates.
  CHECK(render_json(q) == R"({"id":1,"scope_kind":"global","scope_id":null,"title":"first question","body":null,)"
                          R"("status":"open","answer_body":null,"answered_at":null,)"
                          R"("created_at":"2026-08-26T09:31:08.323Z","updated_at":"2026-08-26T09:31:08.323Z"})");

  // The same row with an EMPTY body renders `""`, not `null`. This is the
  // discrimination a raw-SQL row snapshot cannot make on the READ side.
  q.body = std::string{};
  CHECK(render_json(q).contains(R"("body":"")"));
  CHECK_FALSE(render_json(q).contains(R"("body":null)"));
}

TEST_CASE("render_json escapes text rather than emitting invalid JSON", "[question][render]") {
  const question q{
      .id         = 7,
      .scope_kind = question_scope_kind::global,
      .title      = R"(a "quoted" \ title)",
      .body       = "line1\nline2",
      .status     = question_status::open,
      .created_at = "t",
      .updated_at = "t",
  };
  const auto out = render_json(q);
  CHECK(out.contains(R"("title":"a \"quoted\" \\ title")"));
  CHECK(out.contains(R"("body":"line1\nline2")"));
  // A raw newline inside the object would make the "single-line JSON
  // object" contract a lie and break every NDJSON consumer.
  CHECK(out.find('\n') == std::string::npos);
}

TEST_CASE("render_list_text's EMPTY case is `(no questions)`, not empty bytes", "[question][render][empty]") {
  CHECK(render_list_text({}) == "(no questions)\n");
  CHECK(render_list_json({}) == "[]");
}

TEST_CASE("render_list_text is a THREE-column table with the oracle's widths", "[question][render]") {
  const std::vector<question> rows{
      {.id = 1, .scope_kind = question_scope_kind::global, .title = "first question", .status = question_status::answered},
      {.id = 12345678, .scope_kind = question_scope_kind::global, .title = "wide id", .status = question_status::open},
  };
  // id right-aligned in five; status left-aligned and padded to ten; title
  // unpadded; two spaces between columns. An id wider than five is NOT
  // truncated — it pushes the rest of the line right.
  CHECK(render_list_text(rows) == "    1  answered    first question\n"
                                  "12345678  open        wide id\n");
}
