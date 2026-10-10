// @file handlers.t.cpp
// @brief In-process tests for `planar-watch`'s ported handlers (plan 996,
// task 6107).
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root; nothing here calls std::getenv.
//
// ORACLE PROVENANCE. Every expected string is a verbatim transcription of
// bytes `zig/zig-out/bin/planar-watch` wrote under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME:
//
//   $Z version                 exit 0, b'planar-watch dev dev zig 0.16.0\n'
//   $Z nosuchverb              exit 1
//                              stdout b'error: unknown subcommand (got nosuchverb) [in: planar-watch]\n'
//                              stderr b'error: UnknownSubcommand\n'
//   $Z completion badshell     exit 2, stdout b''
//                              stderr b"error: unsupported shell 'badshell'; supported: bash, zsh, fish\n"
//   $Z completion              exit 1
//                              stdout b'error: required positional missing: <shell>\n'
//                              stderr b'error: MissingRequiredPositional\n'
//   $Z completion --help       exit 0
//                              b'completion\n\n  Generate the autocompletion script for the specified shell.\n\n'
//                              b'USAGE:\n  completion <shell>\n\n'
//                              b'POSITIONAL ARGUMENTS:\n  <shell>         (string) — Shell: bash, zsh, or fish\n'
//   $Z --help                  exit 0, root page with the long_desc FLUSH LEFT
//
// THE TWO EXIT CODES ON ONE VERB are the discrimination worth keeping: a
// shell value the parser accepted but the handler rejects is 2
// (invalid_input); a positional the parser never saw is 1 (parse error,
// this binary's policy). A port that collapsed this binary's exit mapping
// to a single code — the easiest way to get it wrong — cannot pass both.
//
// ## Break-probes run against this file
//
//   - Swapped `run`'s parse-error mapping to the operator binary's
//     `exit_code_for_parse_error_planar_binary` -> `a bad shell exits 2, a
//     missing positional exits 1` FAILS on the missing-positional half
//     (2 where 1 is required) while the bad-shell half still passes.
//     Restored -> green.
//   - Changed the invalid-shell error kind from `invalid_input` to
//     `generic_failure` -> the SAME case fails on the OTHER half (1 where
//     2 is required). Restored -> green. Two mutations in opposite
//     directions, each caught by a different half, is what makes the pair
//     worth keeping in one case.
//   - Changed `render_version_text("planar-watch", ...)` to the
//     single-argument overload -> `version names THIS binary` FAILS.
//     Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.main;

namespace {

using planar::cmd::watch::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A fixture root plus the environment every case dispatches under.
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_cmd_watch_h_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  std::vector<std::string> argv{"planar-watch"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::watch::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::watch::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::watch::root_app();
  auto const         table = planar::cmd::watch::handlers(*tree);
  int const          code  = planar::cmd::watch::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Count the newline-terminated lines in a payload.
/// @param text The payload.
/// @return The line count.
auto count_lines(std::string_view text) -> std::size_t {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

/// @brief Run one statement on a WRITABLE connection, failing the test if
/// it does not succeed.
/// @param conn The connection.
/// @param sql The statement.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

/// @brief Create and populate the fixture's database through a WRITABLE
/// connection.
///
/// The seed goes in through `planar.db` and
/// `planar.engine.runtime.agentactivity` rather than through the binary
/// under test, and that is not incidental: `planar-watch` CANNOT create
/// this state, which is the property the whole file exists to check. A
/// fixture the subject could have built itself would prove nothing.
///
/// Two claims on two tasks under one association-scoped plan, on two
/// different vendors so the filter cases have something to discriminate.
/// @param fx The fixture whose `db_path` to populate.
auto seed_database(const fixture& fx) -> void {
  namespace aa = planar::engine::runtime::agentactivity;

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());

  exec(*conn, "insert into associations (slug, name, kind) values ('project:seed','project:seed','project')");
  exec(*conn, "insert into sessions (vendor) values ('seedvendor')");
  exec(*conn, "insert into plans (scope_kind, scope_id, title, slug, status) "
              "values ('association', 1, 'Seed plan', 'seed-plan', 'active')");
  exec(*conn, "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
              "values ('association', 1, 1, 'First', 'todo', 100)");
  exec(*conn, "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
              "values ('association', 1, 1, 'Second', 'todo', 101)");

  auto const first = aa::acquire_claim(
      *conn, aa::acquire_args{.session_id = 1, .kind = aa::entity_kind::task, .entity_id = 1, .vendor = "seedvendor"});
  REQUIRE(first.has_value());
  auto const second = aa::acquire_claim(
      *conn, aa::acquire_args{.session_id = 1, .kind = aa::entity_kind::task, .entity_id = 2, .vendor = "othervendor"});
  REQUIRE(second.has_value());
}

/// @brief Create a MIGRATED but otherwise empty database.
///
/// Distinct from "no database at all", which `context.t.cpp` covers: this
/// is the state a viewer sees right after `planar init`, and it is where
/// the empty-result sentinels (`(no action chains)`, `claims: 0`) live.
/// @param fx The fixture whose `db_path` to create.
auto seed_empty_database(const fixture& fx) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
}

/// @brief The token of the claim `seed_database` created first.
///
/// Read back rather than remembered, because the token is minted in SQL
/// and a test that hardcoded one would be asserting against its own
/// fiction.
/// @param fx The seeded fixture.
/// @return The claim token.
auto seeded_claim_token(const fixture& fx) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select claim_token from agent_work_claims where vendor = 'seedvendor'");
  REQUIRE(stmt.has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  return stmt->column_text(0);
}

} // namespace

TEST_CASE("planar-watch version names THIS binary and opens no database", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("version");
  auto const got = dispatch(fx, {"version"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // On the read-only binary this negative matters more than elsewhere: the
  // cheapest way to break "a viewer never creates state" is an eager open
  // at startup, and that would be invisible in stdout.
  CHECK_FALSE(got.db_open);

  CHECK(got.out.starts_with("planar-watch dev dev cxx "));
  CHECK_FALSE(got.out.starts_with("planar dev"));
  CHECK(got.out.ends_with(" dev\n"));
  CHECK(std::ranges::count(got.out, ' ') + 1 == 6);
}

TEST_CASE("planar-watch completion emits a distinct script per shell and opens no database", "[cmd][watch][handlers]") {
  auto const fx = make_fixture("completion");

  auto const bash = dispatch(fx, {"completion", "bash"});
  auto const zsh  = dispatch(fx, {"completion", "zsh"});
  auto const fish = dispatch(fx, {"completion", "fish"});

  for (auto const* got : {&bash, &zsh, &fish}) {
    CHECK(got->code == 0);
    CHECK(got->err.empty());
    CHECK_FALSE(got->db_open);
    CHECK_FALSE(got->out.empty());
  }

  // Three DIFFERENT scripts, not one script emitted three times — the
  // failure mode of a shell switch that fell through.
  CHECK(bash.out != zsh.out);
  CHECK(zsh.out != fish.out);
  CHECK(bash.out != fish.out);

  // Generated from THIS binary's tree, per
  // zig/src/cmd/planar-watch/handlers/completion.zig's "per-binary
  // completions without sharing state between the command surfaces".
  CHECK(bash.out.contains("planar-watch"));
  CHECK(bash.out.contains("completion"));

  // NOT oracle-byte-compared: `planar.cliapp.completion` defers flag-VALUE
  // completion (see its module header), so the emitted script differs from
  // the reference's. Nothing here claims otherwise.
}

TEST_CASE("planar-watch completion: a bad shell exits 2, a missing positional exits 1", "[cmd][watch][handlers][exitcode]") {
  auto const fx = make_fixture("shellerr");

  // Handler-level refusal: invalid_input -> 2. The body is the oracle's,
  // verbatim.
  auto const bad = dispatch(fx, {"completion", "badshell"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: unsupported shell 'badshell'; supported: bash, zsh, fish\n");

  // Parser-level refusal: parse_error -> 1 under THIS binary's policy
  // (the operator binary would say 2). One stream, stderr only — decision
  // 1004 (task 6271) moved the parser's message off stdout so it presents
  // identically to the handler refusal above.
  auto const missing = dispatch(fx, {"completion"});
  CHECK(missing.code == 1);
  // Re-baselined onto CLI11's wording by task 6123 and pinned exactly. The
  // pair of exit codes out of ONE verb — 2 for the handler's refusal above,
  // 1 for the parser's here — is the part that cannot be satisfied by a
  // collapsed mapping, and it did not move.
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: shell is required\n");
}

TEST_CASE("planar-watch schema appends the terminator its renderer omits", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("schema");
  auto const got = dispatch(fx, {"schema"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  REQUIRE(got.out.ends_with("\n"));
  CHECK(got.out.find('\n') == got.out.size() - 1);
  CHECK(got.out.contains("\"root\":\"planar-watch\""));
  CHECK(got.out.contains("\"planar-watch completion\""));
  // TASK 6065 inverted the assertion that used to sit here. See the
  // twelve-verb case at the bottom of this file for the measurement.
  CHECK(got.out.contains("\"planar-watch feed\""));
}

TEST_CASE("planar-watch parse failures exit 1 and write stderr only", "[cmd][watch][handlers][exitcode]") {
  auto const fx  = make_fixture("parse");
  auto const got = dispatch(fx, {"nosuchverb"});
  CHECK(got.code == 1);
  // Re-baselined onto CLI11's wording by task 6123 and pinned exactly.
  // Decision 1004 (task 6271) moved the formatted message to stderr and
  // dropped the CamelCase tag: nothing on stdout, exit 1 (NOT the
  // operator binary's 2).
  CHECK(got.out.empty());
  CHECK(got.err == "error: planar-watch: The following argument was not expected: nosuchverb\n");
  CHECK_FALSE(got.db_open);
}

TEST_CASE("planar-watch help leads with the read-only prose block", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("help");
  auto const got = dispatch(fx, {"--help"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  // Task 6123 re-baseline. This case used to pin the `long_desc` branch of
  // the deleted `planar.cli.help` renderer, which emitted FLUSH LEFT where
  // the `desc` branch indented by two — planar-agent's root used the
  // latter, and getting the two backwards shifted every line of the page.
  // `CLI::App` carries ONE description string, so that distinction is gone
  // from the surface entirely (recorded in capability.t.cpp and in
  // `planar.cliapp.schema`'s header). What remains worth pinning is that
  // the OPERATOR-FACING CONTRACT STATEMENT still leads the page: the
  // sentence that tells a reader the handle is SQLITE_OPEN_READONLY and
  // that the verb set is the first line of defense. Losing it in
  // transcription would quietly delete the binary's own statement of what
  // it guarantees.
  CHECK(got.out.starts_with("planar-watch is the human-facing live cockpit for agent activity.\n"));
  CHECK(got.out.contains("(SQLITE_OPEN_READONLY) \xe2\x80\x94 every write SQL string is rejected by"));
  CHECK(got.out.contains("`no write verbs registered` capability boundary."));
  // The three ported verbs are listed; nothing else is.
  CHECK(got.out.contains("SUBCOMMANDS:"));
  CHECK(got.out.contains("version"));
  CHECK(got.out.contains("completion"));
  CHECK(got.out.contains("schema"));

  // A bare invocation used to render this same page and exit 0, pinned
  // here as a declared divergence. Task 6136 removed the divergence: the
  // oracle's bare form is the `feed` verb, not the help page, so a bare
  // invocation is now REWRITTEN to `feed`. On an absent database the
  // read-only open refuses rather than creating it. The `--help` page above is unaffected — that is
  // the branch `inject_default_verb` deliberately leaves alone, and the
  // pairing of the two here is what keeps a future over-broad rewrite
  // from swallowing help. See the `[defaultverb]` cases at the end of this
  // file.
  auto const bare = dispatch(fx, {});
  CHECK(bare.code == 1);
  CHECK(bare.out.empty());
}

// ===========================================================================
// The six read verbs (task 6120)
//
// ORACLE PROVENANCE, as above. Every expected string below was first
// observed by running `zig/zig-out/bin/planar-watch` against a scratch
// database seeded through `zig/zig-out/bin/planar` and
// `zig/zig-out/bin/planar-agent`, and the C++ binary was diffed against it
// over 100 invocations across two fixtures (a minimal one and one carrying
// an expired lease, a terminal claim with a failure category, a
// three-generation action forest, a NULL role, a global-scope entity, a
// long worktree path and three activity summaries straddling the 80-byte
// truncation boundary). What is pinned HERE is the subset a unit test can
// state precisely without the oracle present.
//
// ## Break-probes run against these cases
//
//   - Swapped `context::ensure_db`'s `open_read_only` for `open` -> `the
//     read-only handle is exercised END TO END by a real verb` FAILS on
//     the write-refusal half while the read half still passes. That
//     ordering is the whole design: the read runs FIRST, so a failure on
//     the write half cannot be explained by a dead handle. Restored ->
//     green.
//   - Re-added the `claim_matches` filter to `ps`'s ungrouped text arm ->
//     `ps ignores --vendor in the ungrouped text arm` FAILS. Restored ->
//     green. (That probe is the inverse of the usual one: the MUTANT is
//     the reasonable-looking code, and the test exists to keep the
//     oracle's quirk from being tidied away.)
//   - Moved `claims`' row count below the filter -> `the row count is
//     printed BEFORE the filter runs` FAILS. Restored -> green.
//   - Changed `--follow`'s refusal from `not_implemented` to a silent
//     success -> `--follow is refused loudly, not answered with a single
//     snapshot` FAILS. Restored -> green.
//   - Dropped the `entity_scope` field from `claims --json` -> `claims
//     --json carries entity_scope; log --json does not` FAILS on the
//     first half; dropping it from `log`'s lean shape instead fails the
//     second. Restored -> green.

TEST_CASE("planar-watch: the read-only handle is exercised END TO END by a real verb", "[cmd][watch][handlers][readonly]") {
  // WHAT THIS PROVES, precisely: the handle a READ VERB actually used to
  // answer an invocation is the write-refusing one. Before task 6120 the
  // read-only property was proved only in `context.t.cpp`, against a handle
  // no verb consumed — every read verb was unported. The gap that left is
  // not hypothetical: a handler could have opened its own connection with
  // `db::connection::open`, and every context-level test would still have
  // passed.
  //
  // The ORDER below is load-bearing and is the shape commit 2dba671
  // established:
  //
  //   1. run a real verb and require it RETURNED THE SEEDED DATA, so
  //      everything after this point is known to be talking to a live,
  //      populated database rather than a dead handle;
  //   2. reach for the SAME cached handle the verb used and attempt a real
  //      `insert` AND a real `create table` — both must be refused. Two
  //      statements, because SQLite's read-only refusal covers DML and DDL
  //      by different internal paths;
  //   3. re-count the rows through an INDEPENDENT writable connection, so
  //      "the write was refused" is confirmed by the database's contents
  //      and not only by the driver's return value;
  //   4. check `is_read_only()` LAST, so the LABEL can never stand in for
  //      the BEHAVIOUR. A port that returned `true` from a hardcoded flag
  //      would still fail steps 2 and 3.
  auto const fx = make_fixture("roexec");
  seed_database(fx);

  std::vector<std::string> argv{"planar-watch", "claims", "--json"};
  std::ostringstream       out;
  std::ostringstream       err;
  context                  ctx{std::move(argv),
                               planar::cmd::watch::map_env(fx.vars),
                               fx.root / "proj",
                               std::make_shared<planar::cmd::watch::database>(fx.db_path, err),
                               out,
                               err};
  auto const               tree  = planar::cmd::watch::root_app();
  auto const               table = planar::cmd::watch::handlers(*tree);
  int const                code  = planar::cmd::watch::run(ctx, *tree, table);

  // --- 1. a LIVE READ ------------------------------------------------------
  REQUIRE(code == 0);
  REQUIRE(err.str().empty());
  REQUIRE(ctx.db().opened());
  auto const rendered = out.str();
  REQUIRE(rendered.contains("\"claim_token\":\"" + seeded_claim_token(fx) + "\""));
  REQUIRE(rendered.contains("\"vendor\":\"seedvendor\""));

  // --- 2. the SAME handle refuses to write ---------------------------------
  auto handle = ctx.db().ensure_db();
  REQUIRE(handle.has_value());
  auto const insert_result = (*handle)->execute("insert into sessions (vendor) values ('smuggled')");
  CHECK_FALSE(insert_result.has_value());
  auto const ddl_result = (*handle)->execute("create table smuggled (id integer primary key)");
  CHECK_FALSE(ddl_result.has_value());

  // --- 3. the DATABASE agrees ----------------------------------------------
  {
    auto writable = planar::db::connection::open(fx.db_path.string());
    REQUIRE(writable.has_value());
    auto stmt = writable->prepare("select count(*) from sessions where vendor = 'smuggled'");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 0);

    auto tables = writable->prepare("select count(*) from sqlite_master where name = 'smuggled'");
    REQUIRE(tables.has_value());
    auto table_step = tables->step();
    REQUIRE(table_step.has_value());
    REQUIRE(*table_step == planar::db::step_result::row);
    CHECK(tables->column_int64(0) == 0);
  }

  // --- 4. and only now, the label ------------------------------------------
  CHECK((*handle)->is_read_only());
}

TEST_CASE("planar-watch: every read verb answers from the same cached read-only handle", "[cmd][watch][handlers][readonly]") {
  // The companion to the case above, over the whole verb set: no handler
  // may open its own connection. Proved by IDENTITY — the pointer the
  // handler consumed is the pointer the context caches — which is stronger
  // than "it also worked", because a handler that opened a second
  // WRITABLE connection would produce identical output.
  for (auto const& argv :
       std::vector<std::vector<std::string>>{{"ps"}, {"claims"}, {"actions"}, {"plans"}, {"tree"}, {"log", "--task", "1"}}) {
    auto const fx = make_fixture("rohandle");
    seed_database(fx);

    std::vector<std::string> full{"planar-watch"};
    full.insert(full.end(), argv.begin(), argv.end());
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{std::move(full),
                           planar::cmd::watch::map_env(fx.vars),
                           fx.root / "proj",
                           std::make_shared<planar::cmd::watch::database>(fx.db_path, err),
                           out,
                           err};
    auto const         tree  = planar::cmd::watch::root_app();
    auto const         table = planar::cmd::watch::handlers(*tree);

    INFO("verb: " << argv.front());
    CHECK(planar::cmd::watch::run(ctx, *tree, table) == 0);
    CHECK(err.str().empty());
    REQUIRE(ctx.db().opened());
    auto handle = ctx.db().ensure_db();
    REQUIRE(handle.has_value());
    CHECK((*handle)->is_read_only());
    CHECK_FALSE((*handle)->execute("insert into sessions (vendor) values ('x')").has_value());
  }
}

TEST_CASE("planar-watch: the row count is printed BEFORE the filter runs", "[cmd][watch][handlers]") {
  // A reference-binary quirk, reproduced under D2 and pinned so a later
  // reader does not "correct" it into a silent divergence: the text header
  // comes off the row vector's size and the `--vendor` / `--plan`
  // predicates are applied in the emit loop below it. So `claims: 2` can
  // sit above one line.
  auto const fx = make_fixture("countfilter");
  seed_database(fx);

  auto const all = dispatch(fx, {"claims"});
  REQUIRE(all.code == 0);
  CHECK(all.out.starts_with("claims: 2\n"));
  CHECK(count_lines(all.out) == 3);

  auto const filtered = dispatch(fx, {"claims", "--vendor", "seedvendor"});
  REQUIRE(filtered.code == 0);
  // The COUNT does not move…
  CHECK(filtered.out.starts_with("claims: 2\n"));
  // …but a row is gone.
  CHECK(count_lines(filtered.out) == 2);
  CHECK(filtered.out.contains("seedvendor"));
  CHECK_FALSE(filtered.out.contains("othervendor"));
}

TEST_CASE("planar-watch ps ignores --vendor in the ungrouped text arm", "[cmd][watch][handlers]") {
  // THE MUTANT IS THE REASONABLE CODE. `zig/src/cmd/planar-watch/handlers/
  // ps.zig`'s `emitText` is the one emitter of its four that takes no
  // `args` parameter, so `claimMatches` is unreachable from it and
  // `ps --vendor X` prints every active claim. Found by differential
  // testing, not by reading; confirmed on the reference binary. The JSON
  // and `--group-by` arms DO filter, which is what makes this pin
  // meaningful rather than "the filter is unimplemented".
  auto const fx = make_fixture("psfilter");
  seed_database(fx);

  auto const text = dispatch(fx, {"ps", "--vendor", "seedvendor"});
  REQUIRE(text.code == 0);
  CHECK(text.out.starts_with("active: 2\n"));
  CHECK(text.out.contains("othervendor"));

  // Same flag, JSON arm: filtered.
  auto const json = dispatch(fx, {"ps", "--vendor", "seedvendor", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.contains("\"vendor\":\"seedvendor\""));
  CHECK_FALSE(json.out.contains("\"vendor\":\"othervendor\""));

  // Same flag, grouped text arm: filtered.
  auto const grouped = dispatch(fx, {"ps", "--vendor", "seedvendor", "--group-by", "vendor"});
  REQUIRE(grouped.code == 0);
  CHECK(grouped.out.starts_with("[group: seedvendor]\n"));
  CHECK_FALSE(grouped.out.contains("othervendor"));
}

TEST_CASE("planar-watch: --follow is refused loudly, not answered with a single snapshot", "[cmd][watch][handlers][exitcode]") {
  // The streaming arm needs a SIGINT handler and a poll/wake loop, neither
  // of which exists in this tree. Emitting one snapshot and exiting 0 would
  // look like a working stream to any script that pipes it — the failure
  // would surface as "the stream ended immediately", which is
  // indistinguishable from an idle database. Exit 64 is unambiguous.
  auto const fx = make_fixture("follow");
  seed_database(fx);

  for (auto const& verb : {"ps", "claims", "actions", "plans", "tree"}) {
    auto const got = dispatch(fx, {verb, "--follow"});
    INFO("verb: " << verb);
    CHECK(got.code == 64);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: {}: --follow is not implemented in this build\n", verb));
  }

  // `log` declares no `--follow` at all, matching the oracle — so the flag
  // is a PARSE error there, not a handler refusal, and lands on this
  // binary's exit 1. Decision 1004 (task 6271): the formatted message is
  // on stderr alone, no CamelCase tag.
  auto const on_log = dispatch(fx, {"log", "--task", "1", "--follow"});
  CHECK(on_log.code == 1);
  CHECK(on_log.out.empty());
  CHECK(on_log.err == "error: log: The following argument was not expected: --follow\n");
}

TEST_CASE("planar-watch log requires exactly one filter and says how many it got", "[cmd][watch][handlers][exitcode]") {
  auto const fx = make_fixture("logfilters");
  seed_database(fx);

  auto const none = dispatch(fx, {"log"});
  CHECK(none.code == 2);
  CHECK(none.err == "error: log: exactly one of --task / --plan / --entity / --session / --claim required (got 0)\n");

  auto const two = dispatch(fx, {"log", "--task", "1", "--plan", "1"});
  CHECK(two.code == 2);
  CHECK(two.err == "error: log: exactly one of --task / --plan / --entity / --session / --claim required (got 2)\n");

  // A malformed `--entity` names the OFFENDING VALUE, where `actions`
  // reports only the error tag. That asymmetry is the reference binary's:
  // `log` raises per-cause with its own message, `actions` funnels every
  // emit failure through one `"actions: {errorName}"` site.
  auto const no_colon = dispatch(fx, {"log", "--entity", "nocolon"});
  CHECK(no_colon.code == 2);
  CHECK(no_colon.err == "error: log: --entity expects kind:id (got 'nocolon')\n");

  auto const bad_id = dispatch(fx, {"log", "--entity", "task:abc"});
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: log: --entity id is not an integer ('task:abc')\n");

  auto const actions_bad = dispatch(fx, {"actions", "--entity", "nocolon"});
  CHECK(actions_bad.code == 2);
  CHECK(actions_bad.err == "error: actions: InvalidInput\n");
}

TEST_CASE("planar-watch ps rejects a bad --sort-by / --group-by with exit 1, not 2", "[cmd][watch][handlers][exitcode]") {
  // Note the code: these are exit 1 where `log`'s and `actions`' bad input
  // above is exit 2. Both are handler-level refusals of a flag VALUE, so
  // nothing about the shape of the failure explains the difference — it is
  // the reference binary's mapping, verified per-verb rather than inferred
  // from one of them.
  auto const fx = make_fixture("psvalues");
  seed_database(fx);

  auto const sort = dispatch(fx, {"ps", "--sort-by", "bogus"});
  CHECK(sort.code == 1);
  CHECK(sort.err == "error: ps: --sort-by: accepted values are 'heartbeat' (default) or 'lease'\n");

  auto const group = dispatch(fx, {"ps", "--group-by", "bogus"});
  CHECK(group.code == 1);
  CHECK(group.err == "error: ps: --group-by: accepted values are 'role', 'scope', or 'vendor'\n");

  // `claims --status` is the counterpart that does NOT refuse: an
  // unrecognised value falls back to `active` and exits 0.
  auto const status = dispatch(fx, {"claims", "--status", "bogus"});
  CHECK(status.code == 0);
  CHECK(status.err.empty());
  CHECK(status.out.starts_with("claims: 2\n"));
}

TEST_CASE("planar-watch tree validates --root-session before walking", "[cmd][watch][handlers][exitcode]") {
  auto const fx = make_fixture("treesession");
  seed_database(fx);

  auto const zero = dispatch(fx, {"tree", "--root-session", "0"});
  CHECK(zero.code == 1);
  CHECK(zero.err == "error: tree: --root-session: must be a positive integer\n");

  auto const missing = dispatch(fx, {"tree", "--root-session", "9999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: tree: --root-session 9999: session not found\n");

  // The empty-forest sentinel is a LINE, not zero bytes — a consumer that
  // tested for empty output would read "no chains" as "command produced
  // nothing".
  auto const empty_fx = make_fixture("treeempty");
  seed_empty_database(empty_fx);
  auto const empty = dispatch(empty_fx, {"tree"});
  CHECK(empty.code == 0);
  CHECK(empty.out == "(no action chains)\n");
}

TEST_CASE("planar-watch claims --json carries entity_scope; log --json does not", "[cmd][watch][handlers]") {
  // Two surfaces, one claim row shape, and the difference is deliberate:
  // `ps` and `claims` answer "what is running and where does it live", so
  // they carry the entity's storage scope per row. A `log` entry is already
  // scoped by the `entity` header above it, and `planar-agent`'s payloads
  // pin the lean shape byte-for-byte — emitting the field unconditionally
  // would change bytes those tests already hold.
  auto const fx = make_fixture("scopeshape");
  seed_database(fx);

  auto const claims = dispatch(fx, {"claims", "--json"});
  REQUIRE(claims.code == 0);
  CHECK(claims.out.contains("\"entity_scope\":{\"kind\":\"association\",\"slug\":\"project:seed\"}"));
  // `claims` does NOT carry `latest_action`; only `ps` does.
  CHECK_FALSE(claims.out.contains("\"latest_action\""));

  auto const ps = dispatch(fx, {"ps", "--json"});
  REQUIRE(ps.code == 0);
  CHECK(ps.out.contains("\"entity_scope\":{\"kind\":\"association\",\"slug\":\"project:seed\"}"));
  CHECK(ps.out.contains("\"latest_action\":null"));

  auto const log = dispatch(fx, {"log", "--task", "1", "--json"});
  REQUIRE(log.code == 0);
  CHECK(log.out.contains("\"claim\":{"));
  CHECK_FALSE(log.out.contains("\"entity_scope\""));
  CHECK_FALSE(log.out.contains("\"latest_action\""));
}

TEST_CASE("planar-watch: every read verb's JSON arm is exactly one line", "[cmd][watch][handlers]") {
  // The terminator contract, checked at the surface: each of these
  // renderers returns a COMPLETE payload including its trailing newline and
  // the handler writes it verbatim. A double terminator or a missing one is
  // invisible in a `contains` assertion and breaks a line-oriented
  // consumer.
  auto const fx = make_fixture("terminator");
  seed_database(fx);

  for (auto const& argv : std::vector<std::vector<std::string>>{{"ps", "--json"},
                                                                {"claims", "--json"},
                                                                {"actions", "--json"},
                                                                {"plans", "--json"},
                                                                {"log", "--task", "1", "--json"},
                                                                {"ps", "--group-by", "role", "--json"}}) {
    auto const got = dispatch(fx, argv);
    INFO("argv: " << argv.front());
    REQUIRE(got.code == 0);
    REQUIRE(got.out.ends_with("\n"));
    CHECK(got.out.find('\n') == got.out.size() - 1);
  }
}

TEST_CASE("planar-watch schema catalogs all TWELVE oracle verbs", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("schema12");
  auto const got = dispatch(fx, {"schema"});
  REQUIRE(got.code == 0);
  CHECK_FALSE(got.db_open);
  for (auto const* verb : {"ps", "claims", "actions", "plans", "log", "tree", "version", "completion", "schema"}) {
    INFO("ported verb: " << verb);
    CHECK(got.out.contains(std::format("\"planar-watch {}\"", verb)));
  }
  // TASK 6065. This loop used to be a CHECK_FALSE, on the grounds that "a
  // catalog that named an unported verb would be worse than no catalog:
  // tools/cli_usage_lint.zig validates authored surfaces against it."
  //
  // That reads the lint backwards, and the measurement is in
  // `src/lib/cliapp/schema.t.cpp`'s `[lint-parity]` scope section: the
  // tool reports a `--flag` referenced on a command the binary DOES
  // expose, and SKIPS a command path it cannot resolve at all. So omitting
  // a verb never made the gate stricter about that verb — it removed the
  // verb from the gate. Declaring it is what brings its flags under
  // `make cli-usage-check`.
  //
  // What keeps the catalog honest is the refusal, not the omission. Each
  // of the still-unported ones exits 64 naming itself; parity.t.cpp
  // asserts that directly on the built binary. `feed` is declared here
  // too, but is no longer one of them — task 6039 landed it for real.
  for (auto const* verb : {"feed", "run", "sync-events"}) {
    INFO("declared verb present in the catalog: " << verb);
    CHECK(got.out.contains(std::format("\"planar-watch {}\"", verb)));
  }
  // And the two nested `run` leaves, which no top-level loop would catch.
  CHECK(got.out.contains("\"planar-watch run list\""));
  CHECK(got.out.contains("\"planar-watch run show\""));
  // Non-vacuous: the catalog does not simply contain every string it is
  // asked about.
  CHECK_FALSE(got.out.contains("\"planar-watch nosuchverb\""));
}

// ---------------------------------------------------------------------------
// The default verb (plan 996, task 6136).
// ---------------------------------------------------------------------------
//
// ORACLE PROVENANCE, captured under a scratch DB:
//
//   $Z                      exit 0, the activity feed in human form
//   $Z --json               exit 0, the activity feed as NDJSON
//   $Z --help               exit 0, the ROOT help page (no feed)
//   $Z ps                   exit 0, the ps view (no rewrite)
//   $Z bogusverb            exit 1, unknown subcommand
//
// The oracle's root declares NO flags of its own — its `schema` catalog
// reports `"flags":[]` for `planar-watch` — so `--json` reaching `feed`
// is an ARGV REWRITE, not a root-level declaration. Reproducing it as a
// declaration would have added nine flags to this binary's catalog that
// the oracle's does not have, and `src/cmd/catalog_parity.hpp` compares
// the two byte for byte.
//
// `feed` is landed (task 6039), so the two rewritten rows now reach the
// real handler and exit 0, matching the oracle. Before `feed` landed, the
// rewrite target was declared-but-unported and the same rows exited 64
// from the table-miss arm — the CORRECT refusal at the time, and still the
// shape a future unported default verb should take: what the fix replaced
// was rendering the root help page and exiting 0, a silent success where
// the oracle streams data, which is the exact shape `planar.cliapp.surface`'s
// header calls worse than an absent node.
//
// ## Break-probes run against these four cases
//
//   - Made `inject_default_verb` a pure identity function -> `a bare
//     invocation routes to the feed verb` and `a leading flag routes to
//     the feed verb` both FAIL (exit 0 + help page where 64 is required),
//     while the `--help` and explicit-verb cases still pass. Restored,
//     touched, rebuilt -> green.
//   - Dropped the `--help` / `-h` guard so help requests were rewritten
//     too -> `a help request is not rewritten` FAILS (exit 64 where 0 is
//     required) and the other three still pass. Restored -> green.

TEST_CASE("planar-watch a bare invocation routes to the feed verb", "[cmd][watch][handlers][defaultverb]") {
  auto const fx = make_fixture("bareverb");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // The regression this pins: it used to be exit 0 with the root help page.
  CHECK(got.out.empty());
}

TEST_CASE("planar-watch a leading flag routes to the feed verb", "[cmd][watch][handlers][defaultverb]") {
  auto const fx = make_fixture("flagverb");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"--json"});
  // Was exit 1 `ExtrasError` — the symptom task 6136 was filed on.
  CHECK(got.code == 0);
  CHECK(got.err.empty());
}

TEST_CASE("planar-watch a help request is not rewritten", "[cmd][watch][handlers][defaultverb]") {
  auto const fx  = make_fixture("helpverb");
  auto const got = dispatch(fx, {"--help"});
  CHECK(got.code == 0);
  CHECK(got.out.starts_with("planar-watch is the human-facing live cockpit"));
}

TEST_CASE("planar-watch an explicit verb is left alone", "[cmd][watch][handlers][defaultverb]") {
  auto const fx  = make_fixture("explicitverb");
  auto const got = dispatch(fx, {"version"});
  // `version` IS ported and needs no DB, so a rewrite that swallowed the
  // verb would show up unambiguously as the unported `feed`'s exit 64.
  CHECK(got.code == 0);
  CHECK(got.out.starts_with("planar-watch dev dev cxx "));
  CHECK_FALSE(got.err.contains("feed"));
}

TEST_CASE("planar-watch inject_default_verb leaves an unknown verb for the parser", "[cmd][watch][handlers][defaultverb]") {
  // The oracle leaves a non-flag first token alone whatever it is, and
  // lets the parser report it. A rewrite keyed on a hardcoded verb LIST
  // (which the oracle carries and this port deliberately does not) would
  // behave identically here — that equivalence is why the list was not
  // transcribed, and this case is what would catch getting it wrong.
  std::vector<std::string> const argv{"planar-watch", "bogusverb", "--json"};
  auto const                     out = planar::cmd::watch::inject_default_verb(argv);
  CHECK(out == argv);
}

// ===========================================================================
// Task 6448 — `run list` / `run show` / `sync-events`
// ===========================================================================

namespace {

/// @brief Seed one plan (id 1) and nothing else — the minimum `run list` /
/// `run show` / `sync-events` need as an FK target.
/// @param fx The fixture whose `db_path` to populate.
auto seed_plan_only(const fixture& fx) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  exec(*conn, "insert into associations (slug, name, kind) values ('project:seed','project:seed','project')");
  exec(*conn, "insert into plans (scope_kind, scope_id, title, slug, status) "
              "values ('association', 1, 'Seed plan', 'seed-plan', 'active')");
}

} // namespace

TEST_CASE("planar-watch run list --json unions workflow_runs (wf) and runs (op)", "[cmd][watch][handlers][run]") {
  auto const fx = make_fixture("runlistunion");
  seed_plan_only(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (1, 'wf-name', 'wf-run-1', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', 'running')");
    exec(*conn, "insert into runs (run_uid, plan_id, arm, base_sha, config_hash, started_at, status) "
                "values ('op-uid-1', 1, 'finalize', '', '', '2024-01-02T00:00:00.000Z', 'running')");
  }

  auto const got = dispatch(fx, {"run", "list", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("\"generated_at\":"));
  CHECK(got.out.contains("\"runs\":["));
  CHECK(got.out.contains("\"source\":\"wf\""));
  CHECK(got.out.contains("\"source\":\"op\""));
  CHECK(got.out.contains("\"workflow_name\":\"wf-name\""));
  CHECK(got.out.contains("\"workflow_name\":\"finalize\""));
  // op-source sentinels: pid 0, repo_root "".
  CHECK(got.out.contains("\"pid\":0"));
  CHECK(got.out.contains("\"repo_root\":\"\""));
  // Descending by started_at: the op row (started later) sorts first.
  auto const op_pos = got.out.find("\"source\":\"op\"");
  auto const wf_pos = got.out.find("\"source\":\"wf\"");
  REQUIRE(op_pos != std::string::npos);
  REQUIRE(wf_pos != std::string::npos);
  CHECK(op_pos < wf_pos);
}

TEST_CASE("planar-watch run list --arm restricts to one source", "[cmd][watch][handlers][run]") {
  auto const fx = make_fixture("runlistarm");
  seed_plan_only(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (1, 'wf-only', 'wf-run-2', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', 'running')");
    exec(*conn, "insert into runs (run_uid, plan_id, arm, base_sha, config_hash, started_at, status) "
                "values ('op-uid-2', 1, 'op-only', '', '', '2024-01-02T00:00:00.000Z', 'running')");
  }

  auto const wf_only = dispatch(fx, {"run", "list", "--arm", "wf", "--json"});
  CHECK(wf_only.code == 0);
  CHECK(wf_only.out.contains("\"source\":\"wf\""));
  CHECK_FALSE(wf_only.out.contains("\"source\":\"op\""));

  auto const op_only = dispatch(fx, {"run", "list", "--arm", "op", "--json"});
  CHECK(op_only.code == 0);
  CHECK(op_only.out.contains("\"source\":\"op\""));
  CHECK_FALSE(op_only.out.contains("\"source\":\"wf\""));
}

TEST_CASE("planar-watch run list --status filters both sources", "[cmd][watch][handlers][run]") {
  auto const fx = make_fixture("runliststatus");
  seed_plan_only(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (1, 'wf-running', 'wf-run-3', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', 'running')");
    exec(*conn,
         "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, ended_at, status) "
         "values (1, 'wf-done', 'wf-run-4', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', "
         "'2024-01-01T01:00:00.000Z', 'completed')");
  }

  auto const running = dispatch(fx, {"run", "list", "--status", "running", "--json"});
  CHECK(running.code == 0);
  CHECK(running.out.contains("\"workflow_name\":\"wf-running\""));
  CHECK_FALSE(running.out.contains("\"workflow_name\":\"wf-done\""));

  auto const completed_status = dispatch(fx, {"run", "list", "--status", "completed", "--json"});
  CHECK(completed_status.code == 0);
  CHECK(completed_status.out.contains("\"workflow_name\":\"wf-done\""));
  CHECK_FALSE(completed_status.out.contains("\"workflow_name\":\"wf-running\""));
}

TEST_CASE("planar-watch run list --plan restricts to the seeded plan's run only", "[cmd][watch][handlers][run]") {
  auto const fx = make_fixture("runlistplan");
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    auto applied = planar::db::apply_all(*conn);
    REQUIRE(applied.has_value());
    exec(*conn, "insert into associations (slug, name, kind) values ('project:seed','project:seed','project')");
    exec(*conn, "insert into plans (scope_kind, scope_id, title, slug, status) "
                "values ('association', 1, 'Plan A', 'plan-a', 'active')");
    exec(*conn, "insert into plans (scope_kind, scope_id, title, slug, status) "
                "values ('association', 1, 'Plan B', 'plan-b', 'active')");
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (1, 'wf-a', 'wf-run-a', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', 'running')");
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (2, 'wf-b', 'wf-run-b', 111, '/tmp/wf', '2024-01-01T00:00:00.000Z', 'running')");
  }

  auto const got = dispatch(fx, {"run", "list", "--plan", "1", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("\"wf-a\""));
  CHECK_FALSE(got.out.contains("\"wf-b\""));
}

TEST_CASE("planar-watch run list --arm rejects an unrecognized value with exit 1, not 2",
          "[cmd][watch][handlers][run][exitcode]") {
  // Reproduced from the oracle's own `error.InvalidValue`, which its
  // `exit.zig` does NOT map to the 2 an `InvalidInput` gets — it falls
  // through to the generic `else => 1`.
  auto const fx = make_fixture("runlistbadarm");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"run", "list", "--arm", "bogus"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: run list: --arm must be wf, op, or all (got 'bogus')\n");
}

TEST_CASE("planar-watch run show groups context_records by stage then created_at, id as tiebreak",
          "[cmd][watch][handlers][run]") {
  namespace aa  = planar::engine::runtime::agentactivity;
  auto const fx = make_fixture("runshowstage");
  seed_plan_only(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
                "values ('association', 1, 1, 'First', 'todo', 100)");
    exec(*conn, "insert into sessions (vendor) values ('seedvendor')");
    auto const claim = aa::acquire_claim(
        *conn, aa::acquire_args{.session_id = 1, .kind = aa::entity_kind::task, .entity_id = 1, .vendor = "seedvendor"});
    REQUIRE(claim.has_value());
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, status) "
                "values (1, 'show-wf', 'show-run-1', 111, '/tmp', '2024-01-01T00:00:00.000Z', 'running')");
    // "plan" stage inserted FIRST (lower id) but sorts AFTER "code"
    // alphabetically — the ordering the oracle's `ORDER BY stage asc,
    // created_at asc, id asc` produces, and what a naive "insertion
    // order" or "id order" implementation would get wrong.
    exec(*conn, "insert into context_records (run_id, stage, session_id, claim_id, kind, body, created_at) "
                "values (1, 'plan', 1, 1, 'finding', 'finding from plan stage', '2024-01-01T00:00:01.000Z')");
    exec(*conn, "insert into context_records (run_id, stage, session_id, claim_id, kind, body, created_at) "
                "values (1, 'code', 1, 1, 'risk', 'risk from code stage', '2024-01-01T00:00:02.000Z')");
  }

  auto const got = dispatch(fx, {"run", "show", "1", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("\"run\":"));
  CHECK(got.out.contains("\"context_records\":["));
  auto const code_pos = got.out.find("\"stage\":\"code\"");
  auto const plan_pos = got.out.find("\"stage\":\"plan\"");
  REQUIRE(code_pos != std::string::npos);
  REQUIRE(plan_pos != std::string::npos);
  CHECK(code_pos < plan_pos);
}

TEST_CASE("planar-watch run show with an unknown id fails with not_found, not a crash", "[cmd][watch][handlers][run]") {
  auto const fx = make_fixture("runshowmissing");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"run", "show", "999999", "--json"});
  CHECK(got.code != 0);
  CHECK(got.err == "error: run show: run 999999 not found\n");
}

TEST_CASE("planar-watch run show: a non-integer id is a distinct failure from an unknown id",
          "[cmd][watch][handlers][run][exitcode]") {
  auto const fx = make_fixture("runshowbadid");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"run", "show", "not-a-number", "--json"});
  CHECK(got.code == 2);
  CHECK(got.err == "error: run show: id must be an integer\n");
}

TEST_CASE("planar-watch sync-events --json on an empty table returns sync_events: []", "[cmd][watch][handlers][syncevents]") {
  auto const fx = make_fixture("synceventsempty");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"sync-events", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("\"sync_events\":[]"));
}

TEST_CASE("planar-watch sync-events --outcome narrows; text count matches the filtered rows",
          "[cmd][watch][handlers][syncevents]") {
  auto const fx = make_fixture("synceventsoutcome");
  seed_empty_database(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into sync_events (scope, direction, outcome) values ('workbench', 'push', 'ok')");
    exec(*conn, "insert into sync_events (scope, direction, outcome) values ('workbench', 'pull', 'error')");
  }

  auto const all = dispatch(fx, {"sync-events", "--json"});
  CHECK(all.code == 0);
  CHECK(all.out.contains("\"outcome\":\"ok\""));
  CHECK(all.out.contains("\"outcome\":\"error\""));

  auto const filtered = dispatch(fx, {"sync-events", "--outcome", "error", "--json"});
  CHECK(filtered.code == 0);
  CHECK(filtered.out.contains("\"outcome\":\"error\""));
  CHECK_FALSE(filtered.out.contains("\"outcome\":\"ok\""));

  // Text mode's count reflects the FILTERED set here — unlike `claims`,
  // sync-events counts AFTER applying the filter (see `sync_events`'s own
  // two-pass count-then-print, matching `emitOnce`).
  auto const text = dispatch(fx, {"sync-events", "--outcome", "error"});
  CHECK(text.code == 0);
  CHECK(text.out.starts_with("sync_events: 1\n"));
}

TEST_CASE("planar-watch sync-events --plan / --system / --entity filter through the external_links join",
          "[cmd][watch][handlers][syncevents]") {
  auto const fx = make_fixture("synceventsjoin");
  seed_plan_only(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into external_systems (kind, slug, auth_method, auth_ref) "
                "values ('github-issues', 'gh-seed', 'gh-cli', 'seed')");
    exec(*conn, "insert into external_links (entity_kind, entity_id, system_id, external_id) "
                "values ('plan', 1, 1, 'GH-1')");
    // Linked event (via link_id 1) and an unlinked workbench event.
    exec(*conn, "insert into sync_events (link_id, scope, direction, outcome) values (1, 'external', 'push', 'ok')");
    exec(*conn, "insert into sync_events (scope, direction, outcome) values ('workbench', 'push', 'ok')");
  }

  auto const by_plan = dispatch(fx, {"sync-events", "--plan", "1", "--json"});
  CHECK(by_plan.code == 0);
  CHECK(by_plan.out.contains("\"link_id\":1"));
  CHECK_FALSE(by_plan.out.contains("\"link_id\":null"));

  auto const by_system = dispatch(fx, {"sync-events", "--system", "gh-seed", "--json"});
  CHECK(by_system.code == 0);
  CHECK(by_system.out.contains("\"link_id\":1"));
  CHECK_FALSE(by_system.out.contains("\"link_id\":null"));

  auto const by_entity = dispatch(fx, {"sync-events", "--entity", "plan:1", "--json"});
  CHECK(by_entity.code == 0);
  CHECK(by_entity.out.contains("\"link_id\":1"));
  CHECK_FALSE(by_entity.out.contains("\"link_id\":null"));

  // A plan id that does not match the link excludes the row entirely.
  auto const wrong_plan = dispatch(fx, {"sync-events", "--plan", "999", "--json"});
  CHECK(wrong_plan.code == 0);
  CHECK(wrong_plan.out.contains("\"sync_events\":[]"));
}

TEST_CASE("planar-watch sync-events --entity requires kind:id form", "[cmd][watch][handlers][syncevents][exitcode]") {
  auto const fx = make_fixture("synceventsbadentity");
  seed_empty_database(fx);
  auto const got = dispatch(fx, {"sync-events", "--entity", "noColon", "--json"});
  CHECK(got.code == 2);
  CHECK(got.err == "error: sync-events: InvalidInput\n");
}

TEST_CASE("planar-watch claims shows a lapsed engine-supervised claim as lapsed (engine)", "[cmd][watch][handlers][6489]") {
  // Reconcile leaves an expired ENGINE claim for the engine's own recovery
  // (plan 1033 task 6489), so it stays `status='active'` with a passed
  // lease. Rendered as `active` it would read as live work.
  auto const fx = make_fixture("lapsed_engine");
  seed_database(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    // Both claims expire; only the first is engine-supervised.
    exec(*conn, "update agent_work_claims set lease_expires_at = '2000-01-01T00:00:00.000Z'");
    exec(*conn, "update agent_work_claims set supervisor = 'engine', attempt_id = 'A1' where vendor = 'seedvendor'");
  }
  auto const got = dispatch(fx, {"claims", "--status", "stale"});
  REQUIRE(got.code == 0);
  CHECK(got.out.contains("status:lapsed (engine)  vendor:seedvendor"));
  // The caller claim in the same state keeps its stored status verbatim.
  CHECK(got.out.contains("status:active  vendor:othervendor"));
  CHECK_FALSE(got.out.contains("status:lapsed (engine)  vendor:othervendor"));
}

TEST_CASE("planar-watch claims surfaces supervisor and attempt_id, never omitting the keys", "[cmd][watch][handlers][6493]") {
  auto const fx = make_fixture("supervision_claims");
  seed_database(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "update agent_work_claims set supervisor = 'engine', attempt_id = 'A1' where vendor = 'seedvendor'");
  }
  auto const json = dispatch(fx, {"claims", "--json"});
  REQUIRE(json.code == 0);
  // After `stage`, on every claim: the engine claim names its attempt, and
  // a caller claim says so explicitly rather than dropping the keys.
  CHECK(json.out.contains(R"("stage":null,"supervisor":"engine","attempt_id":"A1"})"));
  CHECK(json.out.contains(R"("stage":null,"supervisor":"caller","attempt_id":null})"));

  auto const text = dispatch(fx, {"claims"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("vendor:seedvendor  supervisor:engine  attempt:A1  token:"));
  // A caller claim's line is exactly the pre-plan-1033 shape.
  CHECK(text.out.contains("vendor:othervendor  token:"));
  CHECK_FALSE(text.out.contains("vendor:othervendor  supervisor:"));
}

TEST_CASE("planar-watch run list/show surface engine, and a plan-less run reads null, not plan 0",
          "[cmd][watch][handlers][6493]") {
  auto const fx = make_fixture("run_engine");
  seed_database(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    // Run 1 is shaped like a pre-00038 row (engine defaulted); run 2 is a
    // plan-less Centurion run, which only migration 00038 allows.
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at) "
                "values (1, 'wf', 'r1', 11, '/r', '2000-01-01T00:00:01.000Z')");
    exec(*conn, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, started_at, engine) "
                "values (null, 'wf', 'r2', 12, '/r', '2000-01-01T00:00:02.000Z', 'centurion')");
  }
  auto const shown = dispatch(fx, {"run", "show", "1"});
  REQUIRE(shown.code == 0);
  CHECK(shown.out.contains("\n  engine:embedded\n"));

  auto const json = dispatch(fx, {"run", "list", "--arm", "wf", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.contains(R"({"id":2,"plan_id":null,)"));
  CHECK(json.out.contains(R"("source":"wf","engine":"centurion"})"));
  CHECK(json.out.contains(R"({"id":1,"plan_id":1,)"));
  CHECK(json.out.contains(R"("source":"wf","engine":"embedded"})"));

  auto const text = dispatch(fx, {"run", "list", "--arm", "wf"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("  run:2  plan:-  status:running  source:wf  engine:centurion  "));
  CHECK(text.out.contains("  run:1  plan:1  status:running  source:wf  engine:embedded  "));
}

TEST_CASE("planar-watch feed names a supervision action kind on its text line", "[cmd][watch][handlers][6493]") {
  auto const fx = make_fixture("feed_supervision");
  seed_database(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    exec(*conn, "insert into agent_actions (session_id, claim_id, action_kind, vendor, started_at, ended_at, outcome) "
                "values (1, 1, 'run_submitted', 'seedvendor', '2999-01-01T00:00:00.000Z', null, null)");
  }
  auto const text = dispatch(fx, {"feed"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("  2999-01-01T00:00:00.000Z  action_started  run_submitted\n"));
  auto const json = dispatch(fx, {"feed", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.contains(R"("action_kind":"run_submitted")"));
}
