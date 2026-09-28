// @file feedback_triage_leaves.t.cpp
// @brief Leaf tests for `planar feedback triage {list,show,set}` (plan 996,
// task 6303).
//
// ## Every byte below was captured, not composed
//
// The transcripts came from `zig/zig-out/bin/planar` in a pinned scratch
// arena (46 probes across four exit codes), and the same script was then run
// against this binary and diffed byte-for-byte. What follows is that
// transcript turned into assertions, so a divergence shows up here rather
// than in a future differential run.
//
// ## The fixture is this family's real cost
//
// `feedback triage set` against an ordinary plan answers
// `DifferentFeedbackPlan` for every input, so a suite built on one is green
// and says nothing. A usable arena needs THREE things a reader would not
// guess from this family's own surface:
//
//   1. An ASSOCIATION. A bare registered project cannot create a plan at
//      all — `plan create` refuses at exit 5 with "project has no
//      association" — so `init` alone is not enough.
//   2. A plan whose slug is the literal `planar-feedback`. The slug is the
//      only thing `findingPlan` looks at; the plan's title, scope and
//      status are all irrelevant.
//   3. For the question arm, a `derives-from` entity link. `question add
//      --plan` writes it, which is worth knowing because questions have no
//      `plan_id` column and the link is the only path to a plan.
//
// `seed_feedback_arena` does all three and the FIRST test asserts the result
// before anything compares an outcome.
//
// ## Refusal ordering is a contract here, not an implementation detail
//
// `feedback triage set` validates in argv order: finding ref, severity,
// disposition, reproduction, `--duplicate-of`. A probe passing a bad ref AND
// a bad severity gets the REF message from the oracle, so the order is
// observable and is pinned below.
//
// Two verb prefixes appear in `set`'s failures and they are NOT
// interchangeable: `entity_scope`'s failures read `feedback finding: <Tag>`
// while the engine's read `feedback triage set: <Tag>`. A missing task
// reports the former.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_fbt_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

auto query_rows(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ';';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

auto triage_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select coalesce(finding_task_id,'-'), coalesce(finding_question_id,'-'), severity, "
                    "disposition, reproduction_status, coalesce(duplicate_of_triage_id,'-'), evidence_summary "
                    "from feedback_triage order by id",
                    7);
}

/// @brief Seed the arena every `feedback triage` case needs.
///
/// Each step is REQUIREd: a silently failing seed would drop every case
/// below into one refusal bucket and the suite would still be green.
void seed_feedback_arena(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--slug", "fbrepo", "--json"}).code == 0);
  // A registered project alone cannot create a plan; it needs to belong to
  // an association first.
  REQUIRE(dispatch(fx, {"assoc", "create", "project:fbrepo", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:fbrepo", (fx.root / "proj").string(), "--json"}).code == 0);
  // The slug is the gate. Plan 2 is the decoy that makes
  // `DifferentFeedbackPlan` reachable.
  REQUIRE(dispatch(fx, {"plan", "create", "Feedback", "--slug", "planar-feedback", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Other", "--slug", "other-plan", "--json"}).code == 0);
  // `--editor=false` is not optional: `task add --editor` DEFAULTS TRUE and
  // would otherwise try to spawn an editor.
  REQUIRE(dispatch(fx, {"task", "add", "Finding A", "--plan", "1", "--editor=false", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Finding B", "--plan", "1", "--editor=false", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Finding C", "--plan", "1", "--editor=false", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Off plan", "--plan", "2", "--editor=false", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "No plan", "--editor=false", "--json"}).code == 0);
  // `question add --plan` writes the derives-from link the question arm
  // resolves its plan through.
  REQUIRE(dispatch(fx, {"question", "add", "Q linked", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Q bare", "--json"}).code == 0);
}

} // namespace

TEST_CASE("the seeded feedback arena has the shape the rest of this file assumes") {
  // FIRST. If this degrades every case below refuses identically and the
  // file goes green while asserting nothing.
  auto const fx = make_fixture("shape");
  seed_feedback_arena(fx);
  auto conn = open_db(fx);

  CHECK(query_rows(conn, "select id, slug from plans order by id", 2) == "1|planar-feedback;2|other-plan");
  CHECK(query_rows(conn, "select id, coalesce(plan_id,'-') from tasks order by id", 2) == "1|1;2|1;3|1;4|2;5|-");
  // Exactly one question carries a derives-from edge; the other carries
  // none. Both halves are load-bearing.
  CHECK(query_rows(conn,
                   "select from_id, to_id from entity_links where from_kind='question' "
                   "and relationship='derives-from' order by from_id",
                   2) == "1|1");
  CHECK(query_rows(conn, "select count(*) from feedback_triage", 1) == "0");
}

TEST_CASE("feedback triage list reports the empty state as a sentence and an empty array") {
  auto const fx = make_fixture("empty");
  seed_feedback_arena(fx);

  auto const text = dispatch(fx, {"feedback", "triage", "list"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out == "(no feedback triage)\n");

  // TERMINATED `[]\n`, not zero bytes — the contrast is `workflow list
  // --json`, whose empty catalog really is zero bytes.
  auto const json = dispatch(fx, {"feedback", "triage", "list", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  CHECK(json.out == "[]\n");
}

TEST_CASE("feedback triage show refuses a malformed finding at exit 2 and a missing one at exit 1") {
  auto const fx = make_fixture("showrefuse");
  seed_feedback_arena(fx);

  // A BARE INTEGER is the refusal a port that declared the positional as an
  // int would get wrong — it would parse and then fail somewhere else.
  auto const bare = dispatch(fx, {"feedback", "triage", "show", "1"});
  CHECK(bare.code == 2);
  CHECK(bare.out.empty());
  CHECK(bare.err == "error: finding must be task:<id> or question:<id>\n");

  for (auto const& raw : {"plan:1", "task:", "task:0", "task:-3", "task:abc", "task:2_"}) {
    INFO("expected the malformed-ref refusal for: " << raw);
    auto const bad = dispatch(fx, {"feedback", "triage", "show", raw});
    CHECK(bad.code == 2);
    CHECK(bad.err == "error: finding must be task:<id> or question:<id>\n");
  }

  // A WELL-FORMED ref with no triage row is a different exit and a
  // different message, and the message echoes the RAW positional.
  auto const missing = dispatch(fx, {"feedback", "triage", "show", "task:999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no triage row for task:999\n");

  // Zig's integer spelling: `task:+2` and `task:1_0` PARSE, so they reach
  // the lookup and report the raw text back rather than being refused at
  // exit 2. This is the pair that distinguishes the two arms above.
  auto const plus = dispatch(fx, {"feedback", "triage", "show", "task:+2"});
  CHECK(plus.code == 1);
  CHECK(plus.err == "error: no triage row for task:+2\n");
  auto const separated = dispatch(fx, {"feedback", "triage", "show", "task:1_0"});
  CHECK(separated.code == 1);
  CHECK(separated.err == "error: no triage row for task:1_0\n");
}

TEST_CASE("feedback triage set validates in argv order, ref before the enums") {
  auto const fx = make_fixture("order");
  seed_feedback_arena(fx);

  // Bad ref AND bad severity: the REF wins. A port that checked enums first
  // would report `unknown severity 'nope'` here and still exit 2, so the
  // code alone does not discriminate — the message does.
  auto const both =
      dispatch(fx, {"feedback", "triage", "set", "1", "--severity", "nope", "--disposition", "nope", "--reproduction", "nope"});
  CHECK(both.code == 2);
  CHECK(both.err == "error: finding must be task:<id> or question:<id>\n");

  // Then each enum in turn, each naming the value it rejected.
  auto const sev = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "nope", "--disposition", "accepted",
                                 "--reproduction", "not-run"});
  CHECK(sev.code == 2);
  CHECK(sev.err == "error: unknown severity 'nope'\n");

  auto const disp = dispatch(
      fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "nope", "--reproduction", "not-run"});
  CHECK(disp.code == 2);
  CHECK(disp.err == "error: unknown disposition 'nope'\n");

  auto const repro = dispatch(
      fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "accepted", "--reproduction", "nope"});
  CHECK(repro.code == 2);
  CHECK(repro.err == "error: unknown reproduction 'nope'\n");

  // The duplicate-of flag has its OWN message, naming the flag rather than
  // the positional.
  auto const dup = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "duplicate",
                                 "--reproduction", "not-run", "--duplicate-of", "plan:9"});
  CHECK(dup.code == 2);
  CHECK(dup.err == "error: --duplicate-of must be task:<id> or question:<id>\n");

  // None of the five refusals wrote anything.
  auto conn = open_db(fx);
  CHECK(triage_rows(conn).empty());
}

TEST_CASE("feedback triage set distinguishes a missing plan, a wrong plan, and a missing finding") {
  auto const fx = make_fixture("plans");
  seed_feedback_arena(fx);

  // Task 5 has no plan.
  auto const orphan = dispatch(fx, {"feedback", "triage", "set", "task:5", "--severity", "high", "--disposition", "accepted",
                                    "--reproduction", "not-run"});
  CHECK(orphan.code == 1);
  CHECK(orphan.err == "error: feedback triage set: MissingFeedbackPlan\n");

  // Task 4 has a real plan with the wrong slug.
  auto const wrong = dispatch(fx, {"feedback", "triage", "set", "task:4", "--severity", "high", "--disposition", "accepted",
                                   "--reproduction", "not-run"});
  CHECK(wrong.code == 1);
  CHECK(wrong.err == "error: feedback triage set: DifferentFeedbackPlan\n");

  // A task that does not exist fails EARLIER, in entity_scope, and carries
  // the OTHER verb prefix. Collapsing the two prefixes changes these bytes.
  auto const missing = dispatch(fx, {"feedback", "triage", "set", "task:999", "--severity", "high", "--disposition", "accepted",
                                     "--reproduction", "not-run"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: feedback finding: NotFound\n");

  // A question with no derives-from edge.
  auto const bare = dispatch(fx, {"feedback", "triage", "set", "question:2", "--severity", "high", "--disposition", "accepted",
                                  "--reproduction", "not-run"});
  CHECK(bare.code == 1);
  CHECK(bare.err == "error: feedback triage set: MissingFeedbackPlan\n");

  // A question with TWO edges is ambiguous — an arm no task can reach.
  REQUIRE(dispatch(fx, {"links", "add", "question:2", "plan:1", "--relationship", "derives-from", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "question:2", "plan:2", "--relationship", "derives-from", "--json"}).code == 0);
  auto const ambiguous = dispatch(fx, {"feedback", "triage", "set", "question:2", "--severity", "high", "--disposition",
                                       "accepted", "--reproduction", "not-run"});
  CHECK(ambiguous.code == 1);
  CHECK(ambiguous.err == "error: feedback triage set: AmbiguousFeedbackPlan\n");

  auto conn = open_db(fx);
  CHECK(triage_rows(conn).empty());
}

TEST_CASE("feedback triage set refuses a cross-scope write at exit 5 and names the entity's scope") {
  auto const fx = make_fixture("scope");
  seed_feedback_arena(fx);

  // The findings are association-scoped; `--scope global` is a genuine
  // mismatch. The advice names the ENTITY's scope, not the operator's.
  auto const refused = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "accepted",
                                     "--reproduction", "not-run", "--scope", "global"});
  CHECK(refused.code == 5);
  CHECK(refused.err == "error: Refusing cross-scope write; pass --scope assoc:project:fbrepo or cd into the right repo.\n");

  // An UNKNOWN slug takes the same path rather than a SlugNotFound: the
  // write scope resolves to the literal flag value and then fails the
  // guard. Captured from the oracle, which does the same.
  auto const unknown = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "accepted",
                                     "--reproduction", "not-run", "--scope", "nosuch"});
  CHECK(unknown.code == 5);
  CHECK(unknown.err == "error: Refusing cross-scope write; pass --scope assoc:project:fbrepo or cd into the right repo.\n");

  auto conn = open_db(fx);
  CHECK(triage_rows(conn).empty());

  // And the PRESENT case: passing the scope the refusal ADVISED succeeds,
  // which is what makes the advice actionable rather than decorative.
  auto const accepted = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "accepted",
                                      "--reproduction", "not-run", "--scope", "assoc:project:fbrepo"});
  CHECK(accepted.code == 0);
  CHECK(triage_rows(conn) == "1|-|high|accepted|not-run|-|<NULL>");
}

TEST_CASE("feedback triage set writes a row and echoes the oracle's detail block") {
  auto const fx = make_fixture("write");
  seed_feedback_arena(fx);

  auto const set = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "high", "--disposition", "accepted",
                                 "--reproduction", "reproduced", "--evidence", "redacted evidence"});
  CHECK(set.code == 0);
  CHECK(set.err.empty());
  CHECK(set.out == "task:1\n"
                   "  severity: high\n"
                   "  disposition: accepted\n"
                   "  reproduction: reproduced\n"
                   "  duplicate-of: -\n"
                   "  evidence: redacted evidence\n");

  auto conn = open_db(fx);
  CHECK(triage_rows(conn) == "1|-|high|accepted|reproduced|-|redacted evidence");

  // Re-running UPDATES rather than inserting a second row.
  auto const again = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "critical", "--disposition", "accepted",
                                   "--reproduction", "reproduced", "--evidence", "redacted evidence"});
  CHECK(again.code == 0);
  CHECK(triage_rows(conn) == "1|-|critical|accepted|reproduced|-|redacted evidence");

  // An omitted --evidence lands as SQL NULL, not the empty string. The row
  // above is the paired present case.
  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "dismissed",
                        "--reproduction", "not-run"})
              .code == 0);
  CHECK(query_rows(conn, "select evidence_summary from feedback_triage where finding_task_id = 2", 1) == "<NULL>");
}

TEST_CASE("feedback triage show renders one row as text and as JSON") {
  auto const fx = make_fixture("show");
  seed_feedback_arena(fx);
  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "question:1", "--severity", "medium", "--disposition", "retained-question",
                        "--reproduction", "inconclusive"})
              .code == 0);

  auto const text = dispatch(fx, {"feedback", "triage", "show", "question:1"});
  CHECK(text.code == 0);
  CHECK(text.out == "question:1\n"
                    "  severity: medium\n"
                    "  disposition: retained-question\n"
                    "  reproduction: inconclusive\n"
                    "  duplicate-of: -\n"
                    "  evidence: -\n");

  // The JSON keys and their ORDER are the wire contract. Timestamps vary,
  // so the payload is matched by prefix and the two time fields by name.
  auto const json = dispatch(fx, {"feedback", "triage", "show", "question:1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.starts_with(R"({"id":1,"finding":"question:1","plan_id":1,"severity":"medium",)"
                             R"("disposition":"retained-question","reproduction_status":"inconclusive",)"
                             R"("duplicate_of":null,"evidence_summary":null,"created_at":")"));
  CHECK(json.out.contains(R"(","updated_at":")"));
  CHECK(json.out.ends_with("}\n"));
}

TEST_CASE("feedback triage set walks the duplicate chain and refuses a cycle") {
  auto const fx = make_fixture("dup");
  seed_feedback_arena(fx);

  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "low", "--disposition", "accepted", "--reproduction",
                        "not-run"})
              .code == 0);

  // duplicate without a target, and a target without duplicate: the SAME
  // error, in both directions, at exit 2.
  auto const no_target = dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "duplicate",
                                       "--reproduction", "not-run"});
  CHECK(no_target.code == 2);
  CHECK(no_target.err == "error: feedback triage set: InvalidInput\n");

  auto const stray = dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "accepted",
                                   "--reproduction", "not-run", "--duplicate-of", "task:1"});
  CHECK(stray.code == 2);
  CHECK(stray.err == "error: feedback triage set: InvalidInput\n");

  // Self-reference is a DIFFERENT tag at a DIFFERENT exit code — the pair
  // that proves InvalidInput above is not simply "any duplicate problem".
  auto const self = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "low", "--disposition", "duplicate",
                                  "--reproduction", "not-run", "--duplicate-of", "task:1"});
  CHECK(self.code == 1);
  CHECK(self.err == "error: feedback triage set: DuplicateCycle\n");

  // An untriaged target is InvalidInput, not NotFound.
  auto const untriaged = dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "duplicate",
                                       "--reproduction", "not-run", "--duplicate-of", "task:3"});
  CHECK(untriaged.code == 2);
  CHECK(untriaged.err == "error: feedback triage set: InvalidInput\n");

  // Build a real two-hop chain: 3 -> 2 -> 1.
  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "duplicate",
                        "--reproduction", "not-run", "--duplicate-of", "task:1"})
              .code == 0);
  auto const three = dispatch(fx, {"feedback", "triage", "set", "task:3", "--severity", "low", "--disposition", "duplicate",
                                   "--reproduction", "not-run", "--duplicate-of", "task:2"});
  CHECK(three.code == 0);
  CHECK(three.out.contains("duplicate-of: task:2\n"));

  // Closing the loop 1 -> 3 -> 2 -> 1 is refused. The table's CHECK only
  // catches one hop, so this is the engine's walk doing the work.
  auto const loop = dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "low", "--disposition", "duplicate",
                                  "--reproduction", "not-run", "--duplicate-of", "task:3"});
  CHECK(loop.code == 1);
  CHECK(loop.err == "error: feedback triage set: DuplicateCycle\n");

  // Task 1 is untouched by the refusal.
  auto conn = open_db(fx);
  CHECK(query_rows(conn,
                   "select disposition, coalesce(duplicate_of_triage_id,'-') from feedback_triage "
                   "where finding_task_id = 1",
                   2) == "accepted|-");
}

TEST_CASE("feedback triage list filters narrow the listing and reject unknown values") {
  auto const fx = make_fixture("list");
  seed_feedback_arena(fx);

  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "task:1", "--severity", "critical", "--disposition", "accepted",
                        "--reproduction", "reproduced"})
              .code == 0);
  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "question:1", "--severity", "medium", "--disposition", "retained-question",
                        "--reproduction", "inconclusive"})
              .code == 0);
  REQUIRE(dispatch(fx, {"feedback", "triage", "set", "task:2", "--severity", "low", "--disposition", "dismissed",
                        "--reproduction", "not-run"})
              .code == 0);

  // Column widths 18 / 8 / 20, each followed by one space. Ordered by plan,
  // then most-recently-updated first.
  auto const all = dispatch(fx, {"feedback", "triage", "list"});
  CHECK(all.code == 0);
  CHECK(all.out == "task:2             low      dismissed            not-run\n"
                   "question:1         medium   retained-question    inconclusive\n"
                   "task:1             critical accepted             reproduced\n");

  // Each filter narrows to a DIFFERENT single row, so an inert filter
  // cannot pass all three.
  auto const low = dispatch(fx, {"feedback", "triage", "list", "--severity", "low"});
  CHECK(low.code == 0);
  CHECK(low.out == "task:2             low      dismissed            not-run\n");

  auto const accepted = dispatch(fx, {"feedback", "triage", "list", "--disposition", "accepted"});
  CHECK(accepted.code == 0);
  CHECK(accepted.out == "task:1             critical accepted             reproduced\n");

  // The question resolves to plan 1 through its entity link, so all three
  // rows carry the same plan.
  auto const by_plan = dispatch(fx, {"feedback", "triage", "list", "--plan", "1"});
  CHECK(by_plan.code == 0);
  CHECK(by_plan.out == all.out);

  // A plan with no findings, and a plan that does not exist, are both the
  // empty listing at exit 0 rather than a refusal.
  auto const other = dispatch(fx, {"feedback", "triage", "list", "--plan", "2"});
  CHECK(other.code == 0);
  CHECK(other.out == "(no feedback triage)\n");
  auto const nonexistent = dispatch(fx, {"feedback", "triage", "list", "--plan", "999"});
  CHECK(nonexistent.code == 0);
  CHECK(nonexistent.out == "(no feedback triage)\n");

  // Unknown filter values ARE refused, which is the contrast with the
  // unknown plan above.
  auto const bad_sev = dispatch(fx, {"feedback", "triage", "list", "--severity", "bogus"});
  CHECK(bad_sev.code == 2);
  CHECK(bad_sev.err == "error: unknown severity 'bogus'\n");
  auto const bad_disp = dispatch(fx, {"feedback", "triage", "list", "--disposition", "bogus"});
  CHECK(bad_disp.code == 2);
  CHECK(bad_disp.err == "error: unknown disposition 'bogus'\n");

  // Nothing above mutated the table.
  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select count(*) from feedback_triage", 1) == "3");
}

TEST_CASE("feedback and feedback triage are GROUPS that fall to the help path") {
  auto const fx = make_fixture("groups");
  seed_feedback_arena(fx);

  // Neither is registered as a handler, so both exit 0 with a help page
  // rather than the exit-64 refusal an unported leaf answers.
  auto const family = dispatch(fx, {"feedback"});
  CHECK(family.code == 0);
  CHECK_FALSE(family.err.contains("not implemented in this build"));

  auto const group = dispatch(fx, {"feedback", "triage"});
  CHECK(group.code == 0);
  CHECK_FALSE(group.err.contains("not implemented in this build"));
}
