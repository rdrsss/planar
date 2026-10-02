// @file at_file_flags.t.cpp
// @brief In-process black-box tests for task 7117: every free-text flag that
// docs/cli-reference.md documents as "May be `@<file>`" must read the file,
// exactly as `artifact update --body` and `task update --body` do (task
// 6848): a value whose first byte is `@` names a file read RAW; an
// unreadable file is refused at exit 2 with `error: read <flag>:
// FileNotFound`, and NOTHING is written (no row, no session, no patch).
//
// Before this task `question add --body @file` (and its siblings below)
// stored the literal `@file` token, silently, at exit 0.
//
// One TEST_CASE per verb, so a probe that reverts one verb's fix fails the
// case that NAMES that verb.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment and database path this
/// file's cases dispatch against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory. Nothing here
/// reads the real environment, so the operator's `~/.planar/planar.db` is
/// unreachable.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_atfile_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Dispatch `args` against the real tree and table inside `fx`.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
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

/// @brief Run a one-value SQL query against the fixture's database.
/// @param fx The fixture.
/// @param sql A query returning one text column in its first row.
/// @return The first column of the first row, `<none>` when no row.
auto scalar(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return "<none>";
  }
  return std::string{stmt->column_text(0)};
}

/// @brief The bytes every success case writes to the `@` file. The leading
/// line makes a literal `@path` store unmistakable, and the interior `@`
/// proves only the FIRST byte is the grammar.
constexpr std::string_view file_bytes = "from the file, mail me at foo@example.com\nsecond line";

/// @brief One verb's contract.
struct at_case {
  /// A tag for the scratch directory.
  std::string tag;
  /// Commands that build the state the verb needs; each must exit 0.
  std::vector<std::vector<std::string>> setup;
  /// The verb under test, given the value for the flag/positional under test.
  std::function<std::vector<std::string>(std::string)> verb;
  /// The refusal label, e.g. `--body`.
  std::string label;
  /// SQL returning a string that changes when ANYTHING the verb writes
  /// changes (row counts, sessions included).
  std::string state_sql;
  /// SQL returning the stored value the verb is meant to set.
  std::string value_sql;
};

/// @brief Drive the three contract clauses for one verb: a missing file
/// refuses at exit 2 and writes nothing; a real file is read; a literal
/// value with a LATER `@` is stored verbatim.
/// @param c The verb's contract.
void check_at_flag(const at_case& c) {
  auto const fx = make_fixture(c.tag);
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  for (auto const& step : c.setup) {
    INFO(step.front());
    REQUIRE(dispatch(fx, step).code == 0);
  }

  auto const before  = scalar(fx, c.state_sql);
  auto const refused = dispatch(fx, c.verb("@nope-7117.md"));
  INFO(refused.err);
  CHECK(refused.code == 2);
  CHECK(refused.err == std::format("error: read {}: FileNotFound\n", c.label));
  CHECK(scalar(fx, c.state_sql) == before);

  auto const path = fx.root / "body-7117.md";
  {
    std::ofstream f(path, std::ios::binary);
    f << file_bytes;
  }
  auto const read = dispatch(fx, c.verb("@" + path.string()));
  INFO(read.err);
  REQUIRE(read.code == 0);
  CHECK(scalar(fx, c.value_sql) == file_bytes);
}

/// @brief The negative control: a value that merely CONTAINS an `@` is
/// stored verbatim. Unaffected by the fix by design.
/// @param c The verb's contract.
void check_literal_at(const at_case& c) {
  auto const fx = make_fixture(c.tag + "lit");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  for (auto const& step : c.setup) {
    REQUIRE(dispatch(fx, step).code == 0);
  }
  REQUIRE(dispatch(fx, c.verb("reach me at foo@example.com")).code == 0);
  CHECK(scalar(fx, c.value_sql) == "reach me at foo@example.com");
}

} // namespace

TEST_CASE("question add --body reads @file", "[cmd][question][body][7117]") {
  at_case const c{
      "qadd",
      {},
      [](std::string v) -> std::vector<std::string> { return {"question", "add", "Q", "--scope", "global", "--body", v}; },
      "--body",
      "select (select count(*) from questions) || '/' || (select count(*) from sessions)",
      "select body from questions where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("scenario add --body reads @file", "[cmd][scenario][body][7117]") {
  at_case const c{
      "scadd",
      {},
      [](std::string v) -> std::vector<std::string> { return {"scenario", "add", "S", "--scope", "global", "--body", v}; },
      "--body",
      "select (select count(*) from test_scenarios) || '/' || (select count(*) from sessions)",
      "select body from test_scenarios where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("decision add --body reads @file", "[cmd][decision][body][7117]") {
  at_case const c{
      "dbody",
      {},
      [](std::string v) -> std::vector<std::string> { return {"decision", "add", "D", "--scope", "global", "--body", v}; },
      "--body",
      "select (select count(*) from decisions) || '/' || (select count(*) from sessions)",
      "select body from decisions where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("decision add --rationale reads @file", "[cmd][decision][rationale][7117]") {
  at_case const c{"drat",
                  {},
                  [](std::string v) -> std::vector<std::string> {
                    return {"decision", "add", "D", "--scope", "global", "--body", "stmt", "--rationale", v};
                  },
                  "--rationale",
                  "select (select count(*) from decisions) || '/' || (select count(*) from sessions)",
                  "select rationale from decisions where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("plan create --summary reads @file", "[cmd][plan][summary][7117]") {
  at_case const c{
      "pcreate",
      {},
      [](std::string v) -> std::vector<std::string> { return {"plan", "create", "P", "--scope", "global", "--summary", v}; },
      "--summary",
      "select count(*) from plans",
      "select summary from plans where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("plan update --summary reads @file", "[cmd][plan][summary][7117]") {
  at_case const c{"pupdate",
                  {{"plan", "create", "P", "--scope", "global", "--summary", "original"}},
                  [](std::string v) -> std::vector<std::string> { return {"plan", "update", "1", "--summary", v}; },
                  "--summary",
                  "select summary from plans where id = 1",
                  "select summary from plans where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("annotate add --body reads @file", "[cmd][annotate][body][7117]") {
  at_case const c{"anadd",
                  {},
                  [](std::string v) -> std::vector<std::string> {
                    return {"annotate", "add", "--anchor-path", "src/a.cpp", "--scope", "global", "--body", v};
                  },
                  "--body",
                  "select count(*) from annotations",
                  "select body from annotations where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("annotate update --body reads @file", "[cmd][annotate][body][7117]") {
  at_case const c{"anupd",
                  {{"annotate", "add", "--anchor-path", "src/a.cpp", "--scope", "global", "--body", "original"}},
                  [](std::string v) -> std::vector<std::string> { return {"annotate", "update", "1", "--body", v}; },
                  "--body",
                  "select body from annotations where id = 1",
                  "select body from annotations where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("capture note <body> reads @file", "[cmd][capture][body][7117]") {
  at_case const c{"capnote",
                  {},
                  [](std::string v) -> std::vector<std::string> { return {"capture", "note", v}; },
                  "<body>",
                  "select (select count(*) from session_entries) || '/' || (select count(*) from sessions)",
                  "select body from session_entries where prefix = 'note' order by id desc limit 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("capture command --outcome reads @file", "[cmd][capture][outcome][7117]") {
  at_case const c{"capcmd",
                  {},
                  [](std::string v) -> std::vector<std::string> { return {"capture", "command", "ls", "--outcome", v}; },
                  "--outcome",
                  "select (select count(*) from session_entries) || '/' || (select count(*) from sessions)",
                  "select body from session_entries where prefix = 'command' order by id desc limit 1"};
  // The stored body composes the command and the outcome, so assert the
  // file's bytes are INSIDE it rather than equal to it.
  auto const fx = make_fixture(c.tag);
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  auto const before  = scalar(fx, c.state_sql);
  auto const refused = dispatch(fx, c.verb("@nope-7117.md"));
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: read --outcome: FileNotFound\n");
  CHECK(scalar(fx, c.state_sql) == before);

  auto const path = fx.root / "outcome-7117.md";
  {
    std::ofstream f(path, std::ios::binary);
    f << file_bytes;
  }
  REQUIRE(dispatch(fx, c.verb("@" + path.string())).code == 0);
  auto const stored = scalar(fx, c.value_sql);
  CHECK(stored.find(file_bytes) != std::string::npos);
  CHECK(stored.find("@" + path.string()) == std::string::npos);
}

TEST_CASE("capture end --summary reads @file", "[cmd][capture][summary][7117]") {
  at_case const c{"capend",
                  {{"capture", "session"}},
                  [](std::string v) -> std::vector<std::string> { return {"capture", "end", "--summary", v}; },
                  "--summary",
                  "select coalesce(summary, '') || '/' || coalesce(ended_at, '') from sessions where id = 1",
                  "select summary from sessions where id = 1"};
  // `capture end` is single-shot, so run each clause on its own fixture.
  {
    auto const fx = make_fixture(c.tag);
    REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
    REQUIRE(dispatch(fx, c.setup.front()).code == 0);
    auto const before  = scalar(fx, c.state_sql);
    auto const refused = dispatch(fx, c.verb("@nope-7117.md"));
    CHECK(refused.code == 2);
    CHECK(refused.err == "error: read --summary: FileNotFound\n");
    CHECK(scalar(fx, c.state_sql) == before);
  }
  {
    auto const fx = make_fixture(c.tag + "ok");
    REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
    REQUIRE(dispatch(fx, c.setup.front()).code == 0);
    auto const path = fx.root / "summary-7117.md";
    {
      std::ofstream f(path, std::ios::binary);
      f << file_bytes;
    }
    REQUIRE(dispatch(fx, c.verb("@" + path.string())).code == 0);
    CHECK(scalar(fx, c.value_sql) == file_bytes);
  }
}

TEST_CASE("capture snapshot --note reads @file", "[cmd][capture][note][7117]") {
  at_case const c{"capsnapn",
                  {},
                  [](std::string v) -> std::vector<std::string> { return {"capture", "snapshot", "--note", v}; },
                  "--note",
                  "select (select count(*) from context_snapshots) || '/' || (select count(*) from sessions)",
                  "select body from context_snapshots where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("capture snapshot <body> reads @file", "[cmd][capture][body][7117]") {
  at_case const c{"capsnapp",
                  {},
                  [](std::string v) -> std::vector<std::string> { return {"capture", "snapshot", v}; },
                  "<body>",
                  "select (select count(*) from context_snapshots) || '/' || (select count(*) from sessions)",
                  "select body from context_snapshots where id = 1"};
  check_at_flag(c);
  check_literal_at(c);
}

TEST_CASE("handoff --note reads @file", "[cmd][handoff][note][7117]") {
  at_case const c{"hnote",
                  {{"task", "add", "T", "--scope", "global", "--next-action", "go"}, {"capture", "session"}},
                  [](std::string v) -> std::vector<std::string> { return {"handoff", "1", "--note", v}; },
                  "--note",
                  "select (select count(*) from context_snapshots) || '/' || (select count(*) from handoffs)",
                  "select body from context_snapshots order by id desc limit 1"};
  check_at_flag(c);
}

TEST_CASE("question answer --answer stays literal (documented: no @file expansion)", "[cmd][question][answer][7117]") {
  // docs/cli-reference.md `question answer` states the flag takes literal
  // text. An answer is routinely an `@handle` reply, so this verb is
  // deliberately NOT given the grammar; this pins that decision.
  auto const fx = make_fixture("qanswer");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Q", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "answer", "1", "--answer", "@nope-7117.md"}).code == 0);
  CHECK(scalar(fx, "select answer_body from questions where id = 1") == "@nope-7117.md");
}
