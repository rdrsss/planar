// @file link_leaf.t.cpp
// @brief In-process tests for the top-level `planar link` leaf (plan 996,
// task 6301).
//
// ## THE FIRST CASE ASSERTS THE FIXTURE
//
// Every other case writes an `external_links` row and then reads it back, so
// a fixture that failed to register its external SYSTEM would make every
// write refuse identically at exit 1 — a uniform failure that a
// count-the-rows check reports the same way as a uniform success of zero.
// The first case pins the system and the three local entities before any
// comparison runs.
//
// ## ORACLE PROVENANCE
//
// Captured from `zig/zig-out/bin/planar` built at this cycle's base, in a
// pinned scratch arena (`PLANAR_DB` under a temp root). Exit codes were read
// from the command itself via command substitution with `2>file`, never
// through a pipe. Replayed as a 20-case differential against the built C++
// binary in a second identically-seeded arena: every case outside
// `--propagate` agreed on stdout, stderr and exit code, and the
// `--propagate` cases diverged exactly as designed (see below).
//
// The captures that decided a shape:
//
//   $Z link task:1 --to jira-demo:DEMO-1
//       -> `linked task:1 → jira-demo:DEMO-1  (link id: 1, read-only
//          reference)`. A U+2192 arrow, TWO spaces before the parenthesis,
//          and the pair rendered SYNC-then-ROLE. The defaults are
//          `read-only reference`, NOT the engine's `two-way mirror`.
//
//   $Z link task:999 --to jira-demo:DEMO-10
//       -> exit 0. There is NO existence check on the local entity; a
//          dangling link is one command away. Oracle defect, reproduced.
//
//   $Z link decision:1 --to jira-demo:DEMO-11
//       -> exit 0, where `ext create --from decision:1` REFUSES. `link`
//          validates against the seven `external_entity_kind` spellings and
//          `ext create` against the four its local read serves, so the two
//          verbs accept genuinely different kind sets.
//
//   $Z link task:1 --to jira-demo:DEMO-1  (again)
//       -> exit 6, `link already exists for task:1 on jira-demo`.
//
//   select count(*) from session_entries  -> 0 after ten successful links.
//       `unlink` writes an audit row and this verb does not.
//
// ## THE `--propagate` DIVERGENCE IS PINNED, NOT PAPERED OVER
//
// The oracle PROPAGATES on this flag — its own header comment claiming the
// flag "refuses with NotImplemented" is false, and that false comment is why
// this leaf was carried as engine-blocked for three cycles. This port
// refuses the flag at exit 64 and, deliberately, BEFORE writing the link
// row. The case at the bottom of this file asserts both halves: the refusal
// AND the absence of the row, because a refusal that had already written
// would make its own "re-run without --propagate" advice fail at exit 6.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.engine.external;
import planar.cmd.planar.context;
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

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_link_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"DEMO_TOKEN", "tok-abc"}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table.
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

/// @brief Read one integer out of the fixture database.
/// @param fx The fixture.
/// @param sql A statement whose first column is the value.
/// @return The value.
auto scalar(const fixture& fx, std::string_view sql) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Register one Jira system and three local entities of DIFFERENT
/// kinds.
///
/// Three kinds rather than one because this verb's kind handling is where
/// its two interesting behaviours live: `question` proves a non-task kind
/// links, and the `decision` seeded here is what makes the
/// `link`-accepts-more-than-`ext create` case real rather than notional.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  // `ext register` moved to `planar-ext` at plan 996, task 6419; seeded
  // directly through the engine here, same as `sync.t.cpp`'s `rig` fixture.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::engine::external::system::register_jira(
                *conn, {.slug = "jira-demo", .base_url = "http://127.0.0.1:9", .project = "DEMO", .auth_env = "DEMO_TOKEN"})
                .has_value());
  }
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Demo task", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Demo question", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Demo decision", "--plan", "1", "--body", "because", "--json"}).code == 0);
}

} // namespace

TEST_CASE("the link fixture registers the system its every case writes against", "[cmd][link][fixture]") {
  // Without the system row, every `link` below refuses at exit 1 with the
  // SAME message — a uniform failure indistinguishable from a uniform
  // success of zero rows unless something asserts the precondition.
  auto const fx = make_fixture("fixture");
  seed(fx);

  CHECK(scalar(fx, "select count(*) from external_systems where slug = 'jira-demo'") == 1);
  CHECK(scalar(fx, "select count(*) from plans") == 1);
  CHECK(scalar(fx, "select count(*) from tasks") == 1);
  CHECK(scalar(fx, "select count(*) from questions") == 1);
  CHECK(scalar(fx, "select count(*) from decisions") == 1);
  // And nothing has been linked yet, so every row counted later is one this
  // file's cases wrote.
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("link renders the arrow line and defaults to read-only reference", "[cmd][link][text]") {
  auto const fx = make_fixture("text");
  seed(fx);

  auto const ran = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-1"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  // U+2192, two spaces before `(`, and SYNC before ROLE.
  CHECK(ran.out == "linked task:1 \xe2\x86\x92 jira-demo:DEMO-1  (link id: 1, read-only reference)\n");

  // The defaults reached the ROW, not just the message. `create_args`
  // defaults to `mirror` / `two-way`; reading those as this verb's defaults
  // would silently widen what a later push may send to the remote.
  CHECK(scalar(fx, "select count(*) from external_links where id = 1 and link_role = 'reference'"
                   " and sync_direction = 'read-only' and last_sync_status = 'never'") == 1);
  // The paired presence for those three absences: an EXPLICIT --role/--sync
  // does reach the row, so the assertion above pins the defaults rather than
  // a handler that ignores both flags.
  auto const explicit_flags =
      dispatch(fx, {"link", "plan:1", "--to", "jira-demo:DEMO-2", "--role", "mirror", "--sync", "two-way"});
  CHECK(explicit_flags.code == 0);
  CHECK(explicit_flags.out == "linked plan:1 \xe2\x86\x92 jira-demo:DEMO-2  (link id: 2, two-way mirror)\n");
  CHECK(scalar(fx, "select count(*) from external_links where id = 2 and link_role = 'mirror'"
                   " and sync_direction = 'two-way'") == 1);
}

TEST_CASE("link --json omits external_url and echoes the raw kind", "[cmd][link][json]") {
  auto const fx = make_fixture("json");
  seed(fx);

  auto const ran = dispatch(fx, {"link", "question:1", "--to", "jira-demo:DEMO-9", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out == R"({"ok":true,"link_id":1,"entity_kind":"question","entity_id":1,"external_id":"DEMO-9","system_id":1})"
                   "\n");
  // No `external_url` key at all — this verb records an EXISTING ticket and
  // never learns its URL, unlike `ext create` which reads one from the
  // provider's response.
  CHECK_FALSE(ran.out.contains("external_url"));
  CHECK(scalar(fx, "select count(*) from external_links where id = 1 and external_url is null") == 1);
}

TEST_CASE("link refuses in four distinct buckets, and each refusal writes nothing", "[cmd][link][refusal]") {
  auto const fx = make_fixture("refusal");
  seed(fx);

  auto const bad_ref = dispatch(fx, {"link", "nonsense", "--to", "jira-demo:DEMO-4"});
  CHECK(bad_ref.code == 2);
  CHECK(bad_ref.err == "error: invalid entity ref 'nonsense'; expected kind:integer-id\n");

  auto const bad_to = dispatch(fx, {"link", "task:1", "--to", "nonsense"});
  CHECK(bad_to.code == 2);
  CHECK(bad_to.err == "error: invalid --to value 'nonsense'; expected <system-slug>:<external-id>\n");

  auto const no_system = dispatch(fx, {"link", "task:1", "--to", "no-such:DEMO-5"});
  CHECK(no_system.code == 1);
  CHECK(no_system.err == "error: external system 'no-such' not found\n");

  auto const bad_role = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-6", "--role", "bogus"});
  CHECK(bad_role.code == 2);
  CHECK(bad_role.err == "error: invalid --role 'bogus'\n");

  auto const bad_sync = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-7", "--sync", "bogus"});
  CHECK(bad_sync.code == 2);
  CHECK(bad_sync.err == "error: invalid --sync 'bogus'\n");

  auto const bad_kind = dispatch(fx, {"link", "repo:1", "--to", "jira-demo:DEMO-8"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: unsupported entity kind 'repo'\n");

  // SIX refusals and not one row. Asserting the absence is what separates
  // "refused" from "wrote the row and then printed an error".
  CHECK(scalar(fx, "select count(*) from external_links") == 0);

  // The paired presence: the same shape SUCCEEDS once the invalid part is
  // corrected, so the zero above is a refusal and not a broken fixture.
  CHECK(dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-6", "--role", "mirror"}).code == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
}

TEST_CASE("an external id containing a colon proves --to splits on the FIRST one", "[cmd][link][parse][to]") {
  // THE ONLY INPUT IN THIS FILE THAT SEES `--to`'s SPLIT DIRECTION, and it
  // exists because a break-probe that reversed `find` to `rfind` SURVIVED
  // the entire suite without it. Every other `--to` value here carries
  // exactly one colon, which both directions split identically.
  //
  //   first-colon (correct): slug `jira-demo`, external id `DEMO:5`, exit 0
  //   last-colon:            slug `jira-demo:DEMO`, unregistered, exit 1
  //
  // This is the OPPOSITE of `handlers/sync.cppm`'s finding, where the scan
  // direction is unobservable because no entity KIND contains a colon. An
  // external id legitimately may, so here the direction is load-bearing and
  // this case is what makes it so.
  auto const fx = make_fixture("colonid");
  seed(fx);

  auto const ran = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO:5"});
  CHECK(ran.code == 0);
  CHECK(ran.out == "linked task:1 \xe2\x86\x92 jira-demo:DEMO:5  (link id: 1, read-only reference)\n");
  // The whole tail after the first colon reached the row, not a truncation.
  CHECK(scalar(fx, "select count(*) from external_links where external_id = 'DEMO:5'") == 1);
  // And the slug resolved to the REGISTERED system rather than a mangled
  // one — the half a last-colon split would break.
  CHECK(scalar(fx, "select count(*) from external_links l join external_systems s on s.id = l.system_id"
                   " where l.id = 1 and s.slug = 'jira-demo'") == 1);
}

TEST_CASE("a duplicate link is exit 6 while the same entity on a NEW id is exit 0", "[cmd][link][duplicate]") {
  auto const fx = make_fixture("duplicate");
  seed(fx);

  CHECK(dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-1"}).code == 0);

  auto const again = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-1"});
  CHECK(again.code == 6);
  CHECK(again.err == "error: link already exists for task:1 on jira-demo\n");

  // The UNIQUE is over (kind, id, system, EXTERNAL_ID), so the same entity
  // may carry several links on one system. Without this half, a handler that
  // refused any second link for an entity would pass the case above.
  auto const other = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-3"});
  CHECK(other.code == 0);
  CHECK(scalar(fx, "select count(*) from external_links where entity_kind = 'task' and entity_id = 1") == 2);
}

TEST_CASE("link validates the ROW, not just the kind — no dangling link", "[cmd][link][6314]") {
  // INVERTED AT TASK 6314 (decision 1067). This case used to assert the
  // opposite: `link task:999` returned exit 0 and wrote an `external_links`
  // row pointing at a task that does not exist. Only the KIND was checked.
  //
  // Nothing downstream re-checked. `sync push` / `sync pull` resolve the
  // local endpoint FROM this row, so a typo'd id produced a link that could
  // never sync, and the verb reported success while creating it.
  auto const fx = make_fixture("dangling");
  seed(fx);

  CHECK(scalar(fx, "select count(*) from tasks where id = 999") == 0);
  auto const ran = dispatch(fx, {"link", "task:999", "--to", "jira-demo:DEMO-10"});
  CHECK(ran.code != 0);
  CHECK(ran.err.contains("no task with id 999"));
  // And nothing was written.
  CHECK(scalar(fx, "select count(*) from external_links where entity_id = 999") == 0);

  // `decision` links here and REFUSES under `ext create`, whose local read
  // serves only four kinds. The two verbs' kind sets are independent — and
  // an EXISTING decision still links, so the new check did not narrow the
  // kind set by accident.
  auto const decision = dispatch(fx, {"link", "decision:1", "--to", "jira-demo:DEMO-11"});
  INFO("decision stderr: " << decision.err);
  CHECK(decision.code == 0);
}

TEST_CASE("link's row check is per-KIND, not a single table", "[cmd][link][6314]") {
  // Non-vacuity: a check that always probed `tasks` would pass the case
  // above and still wave through `question:999`. Each kind must resolve
  // against its own table.
  auto const fx = make_fixture("perkind");
  seed(fx);

  auto const q = dispatch(fx, {"link", "question:999", "--to", "jira-demo:DEMO-20"});
  CHECK(q.code != 0);
  CHECK(q.err.contains("no question with id 999"));

  auto const a = dispatch(fx, {"link", "artifact:999", "--to", "jira-demo:DEMO-21"});
  CHECK(a.code != 0);
  CHECK(a.err.contains("no artifact with id 999"));

  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("the --scope flag is accepted and inert, and no audit row is written", "[cmd][link][scope][audit]") {
  auto const fx = make_fixture("scopeaudit");
  seed(fx);

  // Declared so `--help` matches the oracle, read nowhere: `external_links`
  // has no scope column and link verbs are unguarded by design.
  auto const scoped = dispatch(fx, {"link", "task:1", "--to", "jira-demo:DEMO-12", "--scope", "global"});
  CHECK(scoped.code == 0);
  CHECK(scoped.out == "linked task:1 \xe2\x86\x92 jira-demo:DEMO-12  (link id: 1, read-only reference)\n");

  // `unlink` appends a best-effort `session_entries` row; this verb does
  // not, and adding one "for symmetry" would invent a trail entry no
  // reference binary emits.
  CHECK(scalar(fx, "select count(*) from session_entries") == 0);
}

TEST_CASE("the --propagate flag refuses at exit 64 BEFORE writing, so its named remedy still works",
          "[cmd][link][propagate][divergence]") {
  // THE RECORDED DIVERGENCE. The oracle writes the row and then runs full
  // propagation; `ext propagate` is unported, so this port refuses the flag
  // rather than accepting and ignoring it — the defect class that no exit
  // code or stdout diff catches.
  auto const fx = make_fixture("propagate");
  seed(fx);

  auto const ran = dispatch(fx, {"link", "plan:1", "--to", "jira-demo:DEMO-20", "--propagate"});
  CHECK(ran.code == 64);
  CHECK(ran.out.empty());
  CHECK(ran.err == "error: link --propagate: not implemented in this build (the `ext propagate` surface is unported); "
                   "re-run without --propagate\n");

  // The placement half, and the reason it is not merely tidier: NO row was
  // written. Had the refusal come after the write, the remedy in its own
  // message would refuse at exit 6 on the retry.
  CHECK(scalar(fx, "select count(*) from external_links") == 0);

  // And the remedy actually works — the paired presence that makes the
  // absence above meaningful rather than a verb that refuses everything.
  auto const retry = dispatch(fx, {"link", "plan:1", "--to", "jira-demo:DEMO-20"});
  CHECK(retry.code == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
}
