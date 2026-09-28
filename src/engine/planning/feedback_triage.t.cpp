// @file feedback_triage.t.cpp
// @brief Unit tests for `planar.engine.planning.feedback_triage` (plan 996,
// task 6303).
//
// ## The fixture is the hard part of this family, so it is asserted first
//
// `finding_plan` compares the finding's plan slug against the literal
// `planar-feedback`. Against an ordinary plan EVERY interesting arm of this
// module refuses with `different_feedback_plan` — the same uninteresting
// answer for a self-inflicted reason — so a suite built on a plain plan can
// be entirely green and entirely vacuous. The FIRST test below therefore
// asserts the fixture's shape (which plan carries the slug, which tasks hang
// off it, which question carries the `derives-from` edge) before any test
// compares an outcome, so a fixture that silently degrades fails loudly
// rather than collapsing every case into one bucket.
//
// ## Rules inherited from question.t.cpp, and why they bite here
//
//   1. **Row assertions go through raw SQL**, never through
//      `show_feedback_triage`. Two columns in this table are nullable
//      (`evidence_summary`, `duplicate_of_triage_id`) and the write path
//      picks `bind_null` over `bind_text` for the absent case; a test that
//      reads back through the same decoder cannot tell SQL NULL from `''`.
//      `sql_text_or_null` renders NULL as `<NULL>` so they differ at the
//      assertion.
//   2. **Every filter is proven to EXCLUDE**, by asserting the returned
//      finding SET rather than its size, and by asserting the excluded row
//      still exists in the table afterwards.
//   3. **Absence assertions are paired with the present case.** Asserting
//      "duplicate_of is unset" proves nothing if no row in the fixture ever
//      has it set, so every such case also asserts a row where it IS set.
//
// ## What was captured from the oracle rather than reasoned about
//
// `zig/zig-out/bin/planar` in a pinned scratch arena, 46 probes, diffed
// byte-for-byte against this port. The findings that a reasonable
// implementer would have guessed wrong:
//
//   - The id inside a finding ref takes Zig's `parseInt` spelling, so
//     `task:+2` and `task:1_0` PARSE (as 2 and 10) while `task:2_` does not.
//   - `ambiguous_feedback_plan` is reachable ONLY through a question with
//     two `derives-from` plan links. A task cannot reach it: its plan comes
//     from a scalar column, so the count is structurally always one.
//   - A duplicate target that has no triage row of its own is
//     `invalid_input`, not `not_found`.
//   - `entity_scope` returns an unset optional for a global finding — a
//     success, not an error.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.feedback_triage;

namespace {

using planar::engine::planning::entity_scope;
using planar::engine::planning::feedback_disposition;
using planar::engine::planning::feedback_disposition_from_text;
using planar::engine::planning::feedback_disposition_to_text;
using planar::engine::planning::feedback_finding_kind;
using planar::engine::planning::feedback_finding_ref;
using planar::engine::planning::feedback_reproduction;
using planar::engine::planning::feedback_reproduction_from_text;
using planar::engine::planning::feedback_reproduction_to_text;
using planar::engine::planning::feedback_severity;
using planar::engine::planning::feedback_severity_from_text;
using planar::engine::planning::feedback_severity_to_text;
using planar::engine::planning::feedback_triage;
using planar::engine::planning::feedback_triage_error;
using planar::engine::planning::feedback_triage_list_filter;
using planar::engine::planning::feedback_triage_set_args;
using planar::engine::planning::finding_ref_to_text;
using planar::engine::planning::list_feedback_triage;
using planar::engine::planning::parse_finding_ref;
using planar::engine::planning::render_json;
using planar::engine::planning::render_list_json;
using planar::engine::planning::render_list_text;
using planar::engine::planning::render_text;
using planar::engine::planning::set_feedback_triage;
using planar::engine::planning::show_feedback_triage;

struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_feedback_triage_test_{}_{}.db",
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

void exec(planar::db::connection& conn, std::string_view sql) {
  auto applied = conn.execute(sql);
  REQUIRE(applied.has_value());
}

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

/// @brief The finding refs a list result returned, in the order returned.
///
/// A SET / SEQUENCE rather than a count: a filter that returns the right
/// NUMBER of the wrong rows passes a size assertion.
auto findings_of(std::span<const feedback_triage> rows) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    out.push_back(row.finding);
  }
  return out;
}

/// @brief Seed the canonical feedback fixture.
///
/// Plan 1 carries the slug `planar-feedback` and is the ONLY plan any
/// finding may hang off; plan 2 is the decoy that makes
/// `different_feedback_plan` reachable. Tasks 1-3 are on the feedback plan,
/// task 4 is on the decoy, task 5 has no plan at all. Question 1 carries one
/// `derives-from` edge to the feedback plan; question 2 carries none, and
/// individual tests give it two to reach the ambiguous arm.
void seed(planar::db::connection& conn) {
  exec(conn, "insert into plans(scope_kind,scope_id,title,slug,status) values"
             "('global',null,'Feedback','planar-feedback','draft'),"
             "('global',null,'Other','other-plan','draft')");
  exec(conn, "insert into tasks(scope_kind,scope_id,plan_id,title) values"
             "('global',null,1,'Finding A'),"
             "('global',null,1,'Finding B'),"
             "('global',null,1,'Finding C'),"
             "('global',null,2,'Off plan'),"
             "('global',null,null,'No plan')");
  exec(conn, "insert into questions(scope_kind,scope_id,title) values"
             "('global',null,'Q linked'),('global',null,'Q bare')");
  exec(conn, "insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values"
             "('question',1,'plan',1,'derives-from')");
}

auto task_ref(std::int64_t id) -> feedback_finding_ref {
  return feedback_finding_ref{.kind = feedback_finding_kind::task, .id = id};
}
auto question_ref(std::int64_t id) -> feedback_finding_ref {
  return feedback_finding_ref{.kind = feedback_finding_kind::question, .id = id};
}

auto basic_args(feedback_severity sev, feedback_disposition disp, feedback_reproduction repro) -> feedback_triage_set_args {
  return feedback_triage_set_args{
      .severity = sev, .disposition = disp, .reproduction = repro, .duplicate_of = {}, .evidence = {}};
}

} // namespace

TEST_CASE("the feedback fixture has the shape every other case depends on", "[feedback-triage][fixture]") {
  // FIRST, and deliberately so. If this degrades, every arm below refuses
  // for the same uninteresting reason and the suite is green and vacuous.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The slug is the whole gate. Assert the PRESENT case and the decoy.
  CHECK(sql_text_or_null(conn, "select slug from plans where id = 1") == "planar-feedback");
  CHECK(sql_text_or_null(conn, "select slug from plans where id = 2") == "other-plan");

  // Tasks 1-3 on the feedback plan, 4 on the decoy, 5 on NOTHING. The last
  // one is what makes `missing_feedback_plan` reachable.
  CHECK(sql_int(conn, "select plan_id from tasks where id = 1") == 1);
  CHECK(sql_int(conn, "select plan_id from tasks where id = 3") == 1);
  CHECK(sql_int(conn, "select plan_id from tasks where id = 4") == 2);
  CHECK(sql_text_or_null(conn, "select plan_id from tasks where id = 5") == "<NULL>");

  // Question 1 has exactly ONE derives-from edge and question 2 has NONE.
  // Both halves matter: the ambiguous arm is built by adding a second edge
  // to question 2, so a fixture where it already had one would make that
  // test pass for the wrong reason.
  CHECK(sql_int(conn, "select count(*) from entity_links where from_kind='question' and from_id=1") == 1);
  CHECK(sql_int(conn, "select count(*) from entity_links where from_kind='question' and from_id=2") == 0);

  // And nothing is triaged yet, so every "list is empty" case below is
  // empty because nothing was written, not because the table is missing.
  CHECK(sql_int(conn, "select count(*) from feedback_triage") == 0);
}

TEST_CASE("parse_finding_ref takes Zig's integer spelling, not from_chars'", "[feedback-triage][parse]") {
  // The three spellings that distinguish Zig's `parseInt` from a plain
  // `std::from_chars`, each captured from the oracle: `task:+2` resolves to
  // 2, `task:1_0` to 10, and `task:2_` is refused.
  const auto plus = parse_finding_ref("task:+2");
  REQUIRE(plus.has_value());
  CHECK(plus->kind == feedback_finding_kind::task);
  CHECK(plus->id == 2);

  const auto separated = parse_finding_ref("task:1_0");
  REQUIRE(separated.has_value());
  CHECK(separated->id == 10);

  CHECK_FALSE(parse_finding_ref("task:2_").has_value());
  CHECK_FALSE(parse_finding_ref("task:_2").has_value());

  const auto question = parse_finding_ref("question:7");
  REQUIRE(question.has_value());
  CHECK(question->kind == feedback_finding_kind::question);
  CHECK(question->id == 7);

  // Refusals, all `invalid_input`.
  for (const auto* raw : {"1", "plan:1", "task:", "task:0", "task:-3", "task:abc", "task", ":5", "task:1x"}) {
    INFO("expected refusal for: " << raw);
    const auto parsed = parse_finding_ref(raw);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == feedback_triage_error::invalid_input);
  }

  // Round trip, so the rendered form the table stores is the form that
  // parses back.
  CHECK(finding_ref_to_text(task_ref(12)) == "task:12");
  CHECK(finding_ref_to_text(question_ref(3)) == "question:3");
}

TEST_CASE("every enum round-trips through its wire spelling", "[feedback-triage][enum]") {
  // The hyphenated members are the ones a port gets wrong, so they are
  // named literally rather than generated from the enum.
  CHECK(feedback_disposition_to_text(feedback_disposition::needs_reproduction) == "needs-reproduction");
  CHECK(feedback_disposition_to_text(feedback_disposition::retained_question) == "retained-question");
  CHECK(feedback_disposition_to_text(feedback_disposition::reported_external) == "reported-external");
  CHECK(feedback_reproduction_to_text(feedback_reproduction::not_run) == "not-run");
  CHECK(feedback_reproduction_to_text(feedback_reproduction::not_reproduced) == "not-reproduced");

  // The underscored spellings must NOT parse — that is the direction a
  // copy-paste from the enumerator name breaks.
  CHECK_FALSE(feedback_disposition_from_text("needs_reproduction").has_value());
  CHECK_FALSE(feedback_reproduction_from_text("not_run").has_value());
  CHECK_FALSE(feedback_severity_from_text("Critical").has_value());

  for (const auto sev : {feedback_severity::info, feedback_severity::low, feedback_severity::medium, feedback_severity::high,
                         feedback_severity::critical}) {
    CHECK(feedback_severity_from_text(feedback_severity_to_text(sev)) == sev);
  }
  for (const auto disp :
       {feedback_disposition::untriaged, feedback_disposition::needs_reproduction, feedback_disposition::accepted,
        feedback_disposition::retained_question, feedback_disposition::dismissed, feedback_disposition::reported_external,
        feedback_disposition::duplicate}) {
    CHECK(feedback_disposition_from_text(feedback_disposition_to_text(disp)) == disp);
  }
  for (const auto repro : {feedback_reproduction::not_run, feedback_reproduction::reproduced,
                           feedback_reproduction::not_reproduced, feedback_reproduction::inconclusive}) {
    CHECK(feedback_reproduction_from_text(feedback_reproduction_to_text(repro)) == repro);
  }
}

TEST_CASE("set writes a task finding and stores an absent evidence as SQL NULL", "[feedback-triage][set][null]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The PRESENT case and the ABSENT case, so "evidence is NULL" is not
  // passing merely because nothing in the fixture ever sets it.
  const auto with_evidence =
      set_feedback_triage(conn, task_ref(1),
                          feedback_triage_set_args{.severity     = feedback_severity::high,
                                                   .disposition  = feedback_disposition::accepted,
                                                   .reproduction = feedback_reproduction::reproduced,
                                                   .duplicate_of = {},
                                                   .evidence     = std::optional<std::string>{"redacted evidence"}});
  REQUIRE(with_evidence.has_value());
  CHECK(with_evidence->finding == "task:1");
  CHECK(with_evidence->plan_id == 1);

  const auto without_evidence = set_feedback_triage(
      conn, task_ref(2),
      basic_args(feedback_severity::low, feedback_disposition::dismissed, feedback_reproduction::not_reproduced));
  REQUIRE(without_evidence.has_value());

  // Raw SQL, so `<NULL>` and `''` are distinguishable.
  CHECK(sql_text_or_null(conn, "select evidence_summary from feedback_triage where finding_task_id = 1") == "redacted evidence");
  CHECK(sql_text_or_null(conn, "select evidence_summary from feedback_triage where finding_task_id = 2") == "<NULL>");
  CHECK(sql_text_or_null(conn, "select finding_question_id from feedback_triage where finding_task_id = 1") == "<NULL>");
  CHECK(sql_text_or_null(conn, "select severity from feedback_triage where finding_task_id = 1") == "high");
  CHECK(sql_text_or_null(conn, "select reproduction_status from feedback_triage where finding_task_id = 2") == "not-reproduced");
}

TEST_CASE("set on the same finding twice UPDATES rather than inserting", "[feedback-triage][set][upsert]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  const auto first = set_feedback_triage(
      conn, task_ref(1), basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE(first.has_value());
  const auto first_id      = first->id;
  const auto first_created = first->created_at;

  const auto second = set_feedback_triage(
      conn, task_ref(1),
      basic_args(feedback_severity::critical, feedback_disposition::dismissed, feedback_reproduction::reproduced));
  REQUIRE(second.has_value());

  // Same row, same id, same created_at — the partial unique index on
  // `finding_task_id` is what makes the upsert land on the update arm.
  CHECK(second->id == first_id);
  CHECK(second->created_at == first_created);
  CHECK(sql_int(conn, "select count(*) from feedback_triage") == 1);
  CHECK(sql_text_or_null(conn, "select severity from feedback_triage where finding_task_id = 1") == "critical");
  CHECK(sql_text_or_null(conn, "select disposition from feedback_triage where finding_task_id = 1") == "dismissed");
}

TEST_CASE("a question finding resolves its plan through the derives-from edge", "[feedback-triage][set][question]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  const auto linked = set_feedback_triage(
      conn, question_ref(1),
      basic_args(feedback_severity::medium, feedback_disposition::retained_question, feedback_reproduction::inconclusive));
  REQUIRE(linked.has_value());
  CHECK(linked->finding == "question:1");
  CHECK(linked->plan_id == 1);
  CHECK(sql_text_or_null(conn, "select finding_task_id from feedback_triage where finding_question_id = 1") == "<NULL>");

  // Question 2 has no edge at all -> missing, not different.
  const auto bare = set_feedback_triage(
      conn, question_ref(2), basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE_FALSE(bare.has_value());
  CHECK(bare.error() == feedback_triage_error::missing_feedback_plan);

  // TWO edges -> ambiguous. This arm is reachable ONLY for a question; a
  // task's plan is a scalar column, so its count is structurally one.
  exec(conn, "insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values"
             "('question',2,'plan',1,'derives-from'),('question',2,'plan',2,'derives-from')");
  const auto ambiguous = set_feedback_triage(
      conn, question_ref(2), basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE_FALSE(ambiguous.has_value());
  CHECK(ambiguous.error() == feedback_triage_error::ambiguous_feedback_plan);

  // Neither refusal wrote anything.
  CHECK(sql_int(conn, "select count(*) from feedback_triage where finding_question_id = 2") == 0);
}

TEST_CASE("the feedback plan is identified by slug and nothing else", "[feedback-triage][set][plan]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Task 4 sits on a real, existing plan — it is refused for its SLUG.
  const auto other = set_feedback_triage(
      conn, task_ref(4), basic_args(feedback_severity::high, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE_FALSE(other.has_value());
  CHECK(other.error() == feedback_triage_error::different_feedback_plan);

  // Task 5 has no plan at all — a DIFFERENT error, so the two are not
  // collapsed into one refusal.
  const auto orphan = set_feedback_triage(
      conn, task_ref(5), basic_args(feedback_severity::high, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE_FALSE(orphan.has_value());
  CHECK(orphan.error() == feedback_triage_error::missing_feedback_plan);

  // A task that does not exist at all is `not_found`, a third answer.
  const auto missing = set_feedback_triage(
      conn, task_ref(999), basic_args(feedback_severity::high, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == feedback_triage_error::not_found);

  // Proof the slug is the ONLY thing that mattered above: rename the decoy
  // plan onto the feedback slug (after freeing it) and task 4 now succeeds
  // with no other change to the fixture.
  exec(conn, "update plans set slug='was-feedback' where id=1");
  exec(conn, "update plans set slug='planar-feedback' where id=2");
  const auto now_ok = set_feedback_triage(
      conn, task_ref(4), basic_args(feedback_severity::high, feedback_disposition::accepted, feedback_reproduction::not_run));
  REQUIRE(now_ok.has_value());
  CHECK(now_ok->plan_id == 2);
}

TEST_CASE("duplicate and duplicate_of entail each other in both directions", "[feedback-triage][set][duplicate]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(set_feedback_triage(conn, task_ref(1),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());

  // `duplicate` with no target.
  auto       no_target      = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  const auto missing_target = set_feedback_triage(conn, task_ref(2), no_target);
  REQUIRE_FALSE(missing_target.has_value());
  CHECK(missing_target.error() == feedback_triage_error::invalid_input);

  // A target with a disposition that is not `duplicate` — the SAME error,
  // which is why the oracle expresses this as one inequality.
  auto target_no_dup         = basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run);
  target_no_dup.duplicate_of = task_ref(1);
  const auto stray_target    = set_feedback_triage(conn, task_ref(2), target_no_dup);
  REQUIRE_FALSE(stray_target.has_value());
  CHECK(stray_target.error() == feedback_triage_error::invalid_input);

  // Pointing at itself.
  auto self         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  self.duplicate_of = task_ref(1);
  const auto cycle  = set_feedback_triage(conn, task_ref(1), self);
  REQUIRE_FALSE(cycle.has_value());
  CHECK(cycle.error() == feedback_triage_error::duplicate_cycle);

  // A target that exists but has NO triage row is `invalid_input`, not
  // `not_found` — the target's triage id is what gets stored.
  auto untriaged         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  untriaged.duplicate_of = task_ref(3);
  const auto no_row      = set_feedback_triage(conn, task_ref(2), untriaged);
  REQUIRE_FALSE(no_row.has_value());
  CHECK(no_row.error() == feedback_triage_error::invalid_input);

  // Nothing above wrote a row for task 2.
  CHECK(sql_int(conn, "select count(*) from feedback_triage where finding_task_id = 2") == 0);

  // And the PRESENT case: a well-formed duplicate stores the target's id
  // and renders it back as a finding ref.
  auto good         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  good.duplicate_of = task_ref(1);
  const auto stored = set_feedback_triage(conn, task_ref(2), good);
  REQUIRE(stored.has_value());
  REQUIRE(stored->duplicate_of.has_value());
  CHECK(*stored->duplicate_of == "task:1");
  CHECK(sql_int(conn, "select duplicate_of_triage_id from feedback_triage where finding_task_id = 2") ==
        sql_int(conn, "select id from feedback_triage where finding_task_id = 1"));
  // The non-duplicate row's column stays NULL, pairing the absence case
  // with the presence case above.
  CHECK(sql_text_or_null(conn, "select duplicate_of_triage_id from feedback_triage where finding_task_id = 1") == "<NULL>");
}

TEST_CASE("the duplicate chain is walked to its end before a write", "[feedback-triage][set][duplicate]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(set_feedback_triage(conn, task_ref(1),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());
  auto dup_of_1         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  dup_of_1.duplicate_of = task_ref(1);
  REQUIRE(set_feedback_triage(conn, task_ref(2), dup_of_1).has_value());

  auto dup_of_2         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  dup_of_2.duplicate_of = task_ref(2);
  const auto three      = set_feedback_triage(conn, task_ref(3), dup_of_2);
  REQUIRE(three.has_value());
  REQUIRE(three->duplicate_of.has_value());
  CHECK(*three->duplicate_of == "task:2");

  // Now close the loop: 1 -> 3 -> 2 -> 1. The table's CHECK only catches
  // self-reference at ONE hop, so without the walk this would be storable.
  auto dup_of_3         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  dup_of_3.duplicate_of = task_ref(3);
  const auto looped     = set_feedback_triage(conn, task_ref(1), dup_of_3);
  REQUIRE_FALSE(looped.has_value());
  CHECK(looped.error() == feedback_triage_error::duplicate_cycle);

  // The refusal left task 1 exactly as it was — still `accepted`, still
  // with a NULL target.
  CHECK(sql_text_or_null(conn, "select disposition from feedback_triage where finding_task_id = 1") == "accepted");
  CHECK(sql_text_or_null(conn, "select duplicate_of_triage_id from feedback_triage where finding_task_id = 1") == "<NULL>");
}

TEST_CASE("a duplicate target on a different feedback plan is refused", "[feedback-triage][set][duplicate]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Give the decoy plan a triaged finding by temporarily flying the flag.
  exec(conn, "update plans set slug='was-feedback' where id=1");
  exec(conn, "update plans set slug='planar-feedback' where id=2");
  REQUIRE(set_feedback_triage(conn, task_ref(4),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());
  exec(conn, "update plans set slug='other-plan' where id=2");
  exec(conn, "update plans set slug='planar-feedback' where id=1");

  auto cross         = basic_args(feedback_severity::low, feedback_disposition::duplicate, feedback_reproduction::not_run);
  cross.duplicate_of = task_ref(4);
  const auto refused = set_feedback_triage(conn, task_ref(1), cross);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == feedback_triage_error::different_feedback_plan);
  CHECK(sql_int(conn, "select count(*) from feedback_triage where finding_task_id = 1") == 0);
}

TEST_CASE("show reads one finding and reports not_found for an untriaged one", "[feedback-triage][show]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  const auto missing = show_feedback_triage(conn, task_ref(1));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == feedback_triage_error::not_found);

  REQUIRE(
      set_feedback_triage(conn, task_ref(1),
                          basic_args(feedback_severity::high, feedback_disposition::accepted, feedback_reproduction::reproduced))
          .has_value());

  const auto found = show_feedback_triage(conn, task_ref(1));
  REQUIRE(found.has_value());
  CHECK(found->finding == "task:1");
  CHECK(found->severity == feedback_severity::high);
  CHECK(found->disposition == feedback_disposition::accepted);
  CHECK(found->reproduction_status == feedback_reproduction::reproduced);

  // A question id that collides numerically with the task id must NOT match
  // the task's row — the two columns are separate.
  const auto wrong_kind = show_feedback_triage(conn, question_ref(1));
  REQUIRE_FALSE(wrong_kind.has_value());
  CHECK(wrong_kind.error() == feedback_triage_error::not_found);
}

TEST_CASE("list filters each narrow the result and leave the excluded rows in the table", "[feedback-triage][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(set_feedback_triage(
              conn, task_ref(1),
              basic_args(feedback_severity::critical, feedback_disposition::accepted, feedback_reproduction::reproduced))
              .has_value());
  REQUIRE(set_feedback_triage(
              conn, question_ref(1),
              basic_args(feedback_severity::medium, feedback_disposition::retained_question, feedback_reproduction::inconclusive))
              .has_value());
  REQUIRE(set_feedback_triage(conn, task_ref(2),
                              basic_args(feedback_severity::low, feedback_disposition::dismissed, feedback_reproduction::not_run))
              .has_value());

  const auto all = list_feedback_triage(conn, feedback_triage_list_filter{});
  REQUIRE(all.has_value());
  CHECK(findings_of(*all).size() == 3);

  // Severity: the returned SET, not its size.
  const auto low = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = {}, .severity = feedback_severity::low, .disposition = {}});
  REQUIRE(low.has_value());
  CHECK(findings_of(*low) == std::vector<std::string>{"task:2"});

  const auto critical = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = {}, .severity = feedback_severity::critical, .disposition = {}});
  REQUIRE(critical.has_value());
  CHECK(findings_of(*critical) == std::vector<std::string>{"task:1"});

  // Disposition, including the hyphenated one, which is where a wire-form
  // bug would show.
  const auto retained = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = {}, .severity = {}, .disposition = feedback_disposition::retained_question});
  REQUIRE(retained.has_value());
  CHECK(findings_of(*retained) == std::vector<std::string>{"question:1"});

  // Plan: all three findings resolve to plan 1, INCLUDING the question,
  // which reaches it through the entity link rather than a column.
  const auto by_plan = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = std::optional<std::int64_t>{1}, .severity = {}, .disposition = {}});
  REQUIRE(by_plan.has_value());
  CHECK(findings_of(*by_plan).size() == 3);

  const auto other_plan = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = std::optional<std::int64_t>{2}, .severity = {}, .disposition = {}});
  REQUIRE(other_plan.has_value());
  CHECK(other_plan->empty());

  // An unknown plan is an EMPTY listing, not an error.
  const auto nonexistent = list_feedback_triage(
      conn, feedback_triage_list_filter{.plan_id = std::optional<std::int64_t>{999}, .severity = {}, .disposition = {}});
  REQUIRE(nonexistent.has_value());
  CHECK(nonexistent->empty());

  // Every excluded row survives — a filter that DELETED would also have
  // passed the set assertions above.
  CHECK(sql_int(conn, "select count(*) from feedback_triage") == 3);
}

TEST_CASE("list combines its filters conjunctively", "[feedback-triage][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(set_feedback_triage(conn, task_ref(1),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());
  REQUIRE(set_feedback_triage(conn, task_ref(2),
                              basic_args(feedback_severity::low, feedback_disposition::dismissed, feedback_reproduction::not_run))
              .has_value());

  // Each filter alone matches a row; together they match neither, which an
  // OR-combining bug could not produce.
  const auto low_accepted = list_feedback_triage(
      conn, feedback_triage_list_filter{
                .plan_id = {}, .severity = feedback_severity::low, .disposition = feedback_disposition::accepted});
  REQUIRE(low_accepted.has_value());
  CHECK(findings_of(*low_accepted) == std::vector<std::string>{"task:1"});

  const auto high_accepted = list_feedback_triage(
      conn, feedback_triage_list_filter{
                .plan_id = {}, .severity = feedback_severity::high, .disposition = feedback_disposition::accepted});
  REQUIRE(high_accepted.has_value());
  CHECK(high_accepted->empty());
}

TEST_CASE("list orders by plan, then most-recently-updated, then id", "[feedback-triage][list][order]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(set_feedback_triage(conn, task_ref(1),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());
  REQUIRE(set_feedback_triage(conn, task_ref(2),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());
  REQUIRE(set_feedback_triage(conn, task_ref(3),
                              basic_args(feedback_severity::low, feedback_disposition::accepted, feedback_reproduction::not_run))
              .has_value());

  // Pin the timestamps rather than racing the clock: `updated_at` has
  // millisecond resolution and three writes can land inside one tick.
  exec(conn, "update feedback_triage set updated_at='2026-01-01T00:00:00.000Z' where finding_task_id=1");
  exec(conn, "update feedback_triage set updated_at='2026-01-03T00:00:00.000Z' where finding_task_id=2");
  exec(conn, "update feedback_triage set updated_at='2026-01-02T00:00:00.000Z' where finding_task_id=3");

  const auto rows = list_feedback_triage(conn, feedback_triage_list_filter{});
  REQUIRE(rows.has_value());
  // DESCENDING by updated_at, so the newest is first. Insertion order was
  // 1, 2, 3 — a port that forgot the `desc` would return that instead.
  CHECK(findings_of(*rows) == std::vector<std::string>{"task:2", "task:3", "task:1"});
}

TEST_CASE("entity_scope reports global as an unset optional, not an error", "[feedback-triage][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The seeded findings are global.
  const auto global = entity_scope(conn, task_ref(1));
  REQUIRE(global.has_value());
  CHECK_FALSE(global->has_value());

  // A repo-scoped finding resolves to `repo:<project-slug>` — the PRESENT
  // case that keeps the assertion above from passing vacuously.
  exec(conn, "insert into projects(slug,name,root_path) values('fbrepo','Feedback repo','/tmp/fbrepo')");
  exec(conn, "insert into tasks(scope_kind,scope_id,plan_id,title) values('repo',1,1,'Repo finding')");
  const auto repo_id    = sql_int(conn, "select id from tasks where title='Repo finding'");
  const auto repo_scope = entity_scope(conn, task_ref(repo_id));
  REQUIRE(repo_scope.has_value());
  REQUIRE(repo_scope->has_value());
  CHECK(**repo_scope == "repo:fbrepo");

  // An association-scoped finding takes the `assoc:` prefix instead.
  exec(conn, "insert into associations(slug,name,kind) values('acme','Acme','org')");
  exec(conn, "insert into tasks(scope_kind,scope_id,plan_id,title) values('association',1,1,'Assoc finding')");
  const auto assoc_id    = sql_int(conn, "select id from tasks where title='Assoc finding'");
  const auto assoc_scope = entity_scope(conn, task_ref(assoc_id));
  REQUIRE(assoc_scope.has_value());
  REQUIRE(assoc_scope->has_value());
  CHECK(**assoc_scope == "assoc:acme");

  // A finding that does not exist is `not_found`.
  const auto missing = entity_scope(conn, task_ref(999));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == feedback_triage_error::not_found);
}

TEST_CASE("render_text names every field and dashes the unset ones", "[feedback-triage][render]") {
  // Bytes captured from the oracle, indentation included.
  const feedback_triage row{
      .id                  = 1,
      .finding             = "task:1",
      .plan_id             = 1,
      .severity            = feedback_severity::critical,
      .disposition         = feedback_disposition::accepted,
      .reproduction_status = feedback_reproduction::reproduced,
      .duplicate_of        = {},
      .evidence_summary    = std::optional<std::string>{"redacted evidence"},
      .created_at          = "2026-01-01T00:00:00.000Z",
      .updated_at          = "2026-01-01T00:00:00.000Z",
  };
  CHECK(render_text(row) == "task:1\n"
                            "  severity: critical\n"
                            "  disposition: accepted\n"
                            "  reproduction: reproduced\n"
                            "  duplicate-of: -\n"
                            "  evidence: redacted evidence\n");

  // The mirror: a set duplicate and an unset evidence, so BOTH dash arms
  // and both value arms are exercised.
  feedback_triage dup = row;
  dup.finding         = "task:2";
  dup.disposition     = feedback_disposition::duplicate;
  dup.duplicate_of    = std::optional<std::string>{"task:1"};
  dup.evidence_summary.reset();
  CHECK(render_text(dup) == "task:2\n"
                            "  severity: critical\n"
                            "  disposition: duplicate\n"
                            "  reproduction: reproduced\n"
                            "  duplicate-of: task:1\n"
                            "  evidence: -\n");
}

TEST_CASE("render_list_text pads to the oracle's column widths", "[feedback-triage][render]") {
  // The empty case is a SENTENCE, not zero bytes.
  CHECK(render_list_text({}) == "(no feedback triage)\n");

  const feedback_triage question{
      .id                  = 3,
      .finding             = "question:1",
      .plan_id             = 1,
      .severity            = feedback_severity::medium,
      .disposition         = feedback_disposition::retained_question,
      .reproduction_status = feedback_reproduction::inconclusive,
      .duplicate_of        = {},
      .evidence_summary    = {},
      .created_at          = "2026-01-01T00:00:00.000Z",
      .updated_at          = "2026-01-01T00:00:00.000Z",
  };
  // Widths are 18 / 8 / 20, each followed by one literal space. Captured
  // from the oracle; `retained-question` is 17 characters, so its column
  // still leaves exactly four spaces before `inconclusive`.
  const std::vector<feedback_triage> rows{question};
  CHECK(render_list_text(rows) == "question:1         medium   retained-question    inconclusive\n");
}

TEST_CASE("render_json emits declaration order and distinguishes null from empty", "[feedback-triage][render][json]") {
  const feedback_triage row{
      .id                  = 4,
      .finding             = "task:2",
      .plan_id             = 1,
      .severity            = feedback_severity::low,
      .disposition         = feedback_disposition::duplicate,
      .reproduction_status = feedback_reproduction::not_run,
      .duplicate_of        = std::optional<std::string>{"task:1"},
      .evidence_summary    = {},
      .created_at          = "2026-01-01T00:00:00.000Z",
      .updated_at          = "2026-01-02T00:00:00.000Z",
  };
  CHECK(render_json(row) == R"({"id":4,"finding":"task:2","plan_id":1,"severity":"low","disposition":"duplicate",)"
                            R"("reproduction_status":"not-run","duplicate_of":"task:1","evidence_summary":null,)"
                            R"("created_at":"2026-01-01T00:00:00.000Z","updated_at":"2026-01-02T00:00:00.000Z"})");

  // An EMPTY evidence string must render as `""`, not `null` — the pair
  // that proves the null above is a real null.
  feedback_triage empty_evidence  = row;
  empty_evidence.evidence_summary = std::optional<std::string>{""};
  CHECK(render_json(empty_evidence).contains(R"("evidence_summary":"")"));

  // An unset plan_id renders as a bare null, not "null".
  feedback_triage no_plan = row;
  no_plan.plan_id.reset();
  CHECK(no_plan.plan_id.has_value() == false);
  CHECK(render_json(no_plan).contains(R"("plan_id":null,)"));

  // Escaping goes through json_text rather than raw interpolation.
  feedback_triage quoted  = row;
  quoted.evidence_summary = std::optional<std::string>{R"(he said "no")"};
  CHECK(render_json(quoted).contains(R"("evidence_summary":"he said \"no\"")"));
}

TEST_CASE("render_list_json is a bare array with no trailing newline", "[feedback-triage][render][json]") {
  // The empty case is `[]`, which the caller terminates to the oracle's
  // `[]\n`. Zero bytes would be wrong here — that is `workflow list --json`.
  CHECK(render_list_json({}) == "[]");

  const feedback_triage a{
      .id                  = 1,
      .finding             = "task:1",
      .plan_id             = 1,
      .severity            = feedback_severity::high,
      .disposition         = feedback_disposition::accepted,
      .reproduction_status = feedback_reproduction::reproduced,
      .duplicate_of        = {},
      .evidence_summary    = {},
      .created_at          = "2026-01-01T00:00:00.000Z",
      .updated_at          = "2026-01-01T00:00:00.000Z",
  };
  feedback_triage b = a;
  b.id              = 2;
  b.finding         = "task:2";

  const std::vector<feedback_triage> rows{a, b};
  const auto                         out = render_list_json(rows);
  CHECK(out.starts_with("[{"));
  CHECK(out.ends_with("}]"));
  CHECK_FALSE(out.ends_with("\n"));
  CHECK(out.contains(R"("finding":"task:1")"));
  CHECK(out.contains(R"(},{)")); // separated, not concatenated
  CHECK(out == std::format("[{},{}]", render_json(a), render_json(b)));
}
