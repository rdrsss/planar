// @file search_health_audit_leaves.t.cpp
// @brief In-process tests for the three leaves wired by plan 996, task
// 6090: `planar search`, `planar health hygiene`, `planar audit session`.
//
// ## WHY THESE THREE SHARE A FILE
//
// They are three different families and would normally get three files.
// They share one here because they were ported in one cycle against ONE
// oracle arena, and the cases that earn their keep are the CROSS-FAMILY
// disagreements — the same flag spelling meaning three different things on
// three different verbs. Splitting them would put each half of a
// contradiction somewhere the other half is not, which is how "harmonising"
// mistakes get made.
//
// ## THE EMPTY-STRING FLAG MEANS THREE DIFFERENT THINGS ACROSS THESE VERBS
//
// Every one of these was captured from `zig/zig-out/bin/planar`, not
// inferred, and no two are derivable from each other:
//
//     search --scope ""    -> CROSS-SCOPE search, exit 0, all six kinds.
//                             The handler maps "" to "no scope filter".
//     search --status ""   -> MATCHES NOTHING, exit 0, `(no results)`.
//                             The value goes through verbatim as a
//                             one-element list and `status IN ('')` is
//                             empty. It is NOT "any status".
//     scope show --scope "" -> REFUSES at exit 1 (pinned in
//                             scope_leaves.t.cpp, not here).
//
// The `search` pair is asserted in ONE case rather than two, so the file
// fails if someone makes the two flags agree.
//
// ## `--kind` VALIDATES; `--status` DOES NOT
//
// `--kind bogus` refuses at exit 2 NAMING the value. `--status bogus`
// prints `(no results)` at exit 0. Both are pinned together for the same
// reason: a reader who sees only the second concludes the verb never
// validates, and a reader who sees only the first concludes it always
// does.
//
// ## THE TWO BAD-ID EXIT CODES ON `audit session`
//
//     audit session abc -> exit 2, `session id must be an integer, got 'abc'`
//     audit session 99  -> exit 1, `session 99 not found`
//
// Two shapes of "you gave me a bad id", two different codes. Pinned
// together; a port that collapsed them would still "refuse" on both.
//
// ## TWO JSON NULL CONVENTIONS, ONE CYCLE
//
//     audit session --json   omits unset optionals entirely
//                            (`emit_null_optional_fields = false`), so an
//                            active session has NO `ended_at` key.
//     health hygiene --json  emits `"parent_plan_id":null`.
//
// Same cycle, same tree, opposite conventions, both captured from live
// runs. Asserted as exact payloads rather than as "parses as JSON".
//
// ## `health hygiene` REFUSES IN THREE WAYS AND ALL THREE ARE EXIT 1
//
// Including the negative-threshold one, which reads like a flag-validation
// error and would naturally be given exit 2. It is not: the oracle dies
// through `error.InvalidThreshold`, which has no arm in its `codeFor`
// table, so it lands in the generic bucket. Captured.
//
// ## `health hygiene` DOES NOT RESOLVE A READ SET; `search` DOES
//
// With no `--scope`, `search` resolves the cwd read set and REFUSES at
// exit 1 when it is empty. `health hygiene` applies no scope predicate at
// all and reports every scope in the database. The same absent flag, two
// opposite meanings, one cycle. Both asserted.
//
// ## HOME / DB SAFETY
//
// Every fixture builds an explicit environment map rooted at its own
// scratch directory — `PLANAR_DB`, `PLANAR_HOME`, `PLANAR_LOCAL_HOME` and
// `HOME` all point inside it, and nothing here reads the process
// environment. All three verbs open SQLite, so the pinned `PLANAR_DB` is
// load-bearing: without it these cases would migrate the operator's live
// `~/.planar/planar.db`.
//
// ## FIXTURE NON-EMPTINESS IS ASSERTED, NOT ASSUMED
//
// `search` and `health hygiene` are result-set verbs, so a fixture that
// silently matches nothing makes every case pass vacuously on the empty
// answer. `seed_searchable` therefore CHECKs that six hits come back before
// any case reads one. The specific way that goes wrong here is known:
// `assoc add <slug> .` stores the literal `.` in `projects.root_path`, a
// row no cwd-derivation can ever match, so every path below is passed
// ABSOLUTE.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

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

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_sha_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "outside", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch_at(const fixture& fx, const std::filesystem::path& cwd, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  auto vars   = fx.vars;
  vars["PWD"] = cwd.string();

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(vars), cwd, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  return dispatch_at(fx, fx.root / "proj", std::move(args));
}

/// @brief Seed one row of every searchable kind, plus the association that
/// makes the cwd resolve to a scope.
///
/// Every entity carries the token `zephyr`, which appears nowhere else in
/// this file's expectations — so a case that matches it is matching THIS
/// fixture rather than incidental schema text.
/// @param fx The fixture.
void seed_searchable(const fixture& fx) {
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  // ABSOLUTE, never `.` — see this file's header.
  CHECK(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Zephyr indexing plan", "--summary", "zephyr summary body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "Zephyr task one", "--plan", "1", "--body", "zephyr in task body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"question", "add", "Zephyr question", "--body", "zephyr question body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"decision", "add", "Zephyr decision", "--body", "zephyr decision body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"scenario", "add", "Zephyr scenario", "--body", "zephyr given when then", "--json"}).code == 0);
  CHECK(
      dispatch(fx, {"artifact", "add", "Zephyr artifact", "--kind", "other", "--body", "zephyr artifact body", "--json"}).code ==
      0);

  // THE VACUITY GUARD. Six kinds seeded, six hits expected. Without this a
  // fixture that silently stored nothing would make every case below pass
  // against `(no results)`.
  auto const probe = dispatch(fx, {"search", "zephyr", "--json"});
  CHECK(probe.code == 0);
  std::size_t kinds = 0;
  for (std::size_t at = probe.out.find("\"kind\""); at != std::string::npos; at = probe.out.find("\"kind\"", at + 1)) {
    ++kinds;
  }
  CHECK(kinds == 6);
}

} // namespace

TEST_CASE("search finds one row of every kind and orders ties by kind then id", "[cmd][search]") {
  auto const fx = make_fixture("hits");
  seed_searchable(fx);

  auto const text = dispatch(fx, {"search", "zephyr"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // Every rank is an identical bm25 tie here, so the whole visible order is
  // the `kind ASC, id ASC` tie-break. That is the point of asserting the
  // full payload rather than "contains plan": a port that dropped the
  // tie-break would still contain every line, in an unstable order.
  CHECK(text.out == "artifact:1 [draft] — Zephyr artifact\n"
                    "  <mark>Zephyr</mark> artifact\n"
                    "decision:1 [proposed] — Zephyr decision\n"
                    "  <mark>Zephyr</mark> decision\n"
                    "plan:1 (zephyr-indexing-plan) [draft] — Zephyr indexing plan\n"
                    "  <mark>Zephyr</mark> indexing plan\n"
                    "question:1 [open] — Zephyr question\n"
                    "  <mark>Zephyr</mark> question\n"
                    "scenario:1 [draft] — Zephyr scenario\n"
                    "  <mark>Zephyr</mark> scenario\n"
                    "task:1 [todo] — Zephyr task one\n"
                    "  <mark>Zephyr</mark> task one\n");
}

TEST_CASE("search renders rank in PLAIN DECIMAL, never scientific notation", "[cmd][search][json]") {
  auto const fx = make_fixture("rank");
  seed_searchable(fx);

  auto const json = dispatch(fx, {"search", "zephyr", "--json"});
  CHECK(json.code == 0);
  // The oracle's `{}` on an `f64` is shortest-round-trip digits in FIXED
  // notation. `std::to_chars`'s default is not — it picks whichever of
  // fixed and scientific is shorter, and bm25's magnitudes land squarely in
  // the range where scientific wins (`1.375e-06`). Asserting the absence of
  // `e-` is what fails if someone swaps the hand-rolled formatter for the
  // obvious one; asserting the literal digits is what fails if the
  // formatter is subtly wrong.
  CHECK(json.out.find("e-") == std::string::npos);
  CHECK(json.out.find("\"rank\":0.000001") != std::string::npos);
  CHECK(json.out.ends_with("]\n"));
}

TEST_CASE("search --scope \"\" searches everything while --status \"\" matches nothing", "[cmd][search][empty-flag]") {
  auto const fx = make_fixture("emptyflag");
  seed_searchable(fx);

  // ONE case on purpose. The two flags take the same empty string and mean
  // opposite things; split across two cases, "harmonising" them would leave
  // one green.
  auto const scope_empty = dispatch(fx, {"search", "zephyr", "--scope", ""});
  CHECK(scope_empty.code == 0);
  CHECK(scope_empty.out.find("plan:1") != std::string::npos);
  CHECK(scope_empty.out.find("task:1") != std::string::npos);

  auto const status_empty = dispatch(fx, {"search", "zephyr", "--status", ""});
  CHECK(status_empty.code == 0);
  CHECK(status_empty.out == "(no results)\n");
}

TEST_CASE("search validates --kind but not --status", "[cmd][search][validation]") {
  auto const fx = make_fixture("kindval");
  seed_searchable(fx);

  auto const bad_kind = dispatch(fx, {"search", "zephyr", "--kind", "bogus"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: unknown kind 'bogus'\n");
  CHECK(bad_kind.out.empty());

  // Same shape of typo on the sibling flag, opposite treatment.
  auto const bad_status = dispatch(fx, {"search", "zephyr", "--status", "bogus"});
  CHECK(bad_status.code == 0);
  CHECK(bad_status.out == "(no results)\n");
}

TEST_CASE("search reports FTS5 syntax errors rather than an empty result", "[cmd][search][fts]") {
  auto const fx = make_fixture("fts");
  seed_searchable(fx);

  // An unclosed quote is rejected by FTS5's parser. Returning `(no
  // results)` here would be indistinguishable from a genuine no-match,
  // which is the silent-filter class of defect.
  auto const bad = dispatch(fx, {"search", "\"unclosed"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: invalid FTS5 query syntax\n");
}

TEST_CASE("search refuses from a cwd outside every registered scope", "[cmd][search][scope]") {
  auto const fx = make_fixture("noscope");
  seed_searchable(fx);

  auto const outside = dispatch_at(fx, fx.root / "outside", {"search", "zephyr"});
  CHECK(outside.code == 1);
  CHECK(outside.err ==
        "error: cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global\n");
  // The refusal is what makes this useful: falling through to an
  // unfiltered search would look like success.
  CHECK(outside.out.empty());

  // The remedy the message names actually works from the same cwd.
  auto const with_scope = dispatch_at(fx, fx.root / "outside", {"search", "zephyr", "--scope", "global"});
  CHECK(with_scope.code == 0);
  CHECK(with_scope.out == "(no results)\n");
}

TEST_CASE("search --plan filters tasks by column and everything else by link", "[cmd][search][plan]") {
  auto const fx = make_fixture("planfilter");
  seed_searchable(fx);

  // `task:1` carries `plan_id = 1` as a COLUMN. The plan itself, and the
  // four link-anchored kinds, have no `derives-from` edge to plan 1, so
  // they drop out. The asymmetry is the schema's, not a bug.
  auto const scoped = dispatch(fx, {"search", "zephyr", "--plan", "1"});
  CHECK(scoped.code == 0);
  CHECK(scoped.out == "task:1 [todo] — Zephyr task one\n"
                      "  <mark>Zephyr</mark> task one\n");
}

TEST_CASE("search --limit caps the MERGED result, and 0 means the default", "[cmd][search][limit]") {
  auto const fx = make_fixture("limit");
  seed_searchable(fx);

  auto const two = dispatch(fx, {"search", "zephyr", "--limit", "2"});
  CHECK(two.code == 0);
  CHECK(two.out == "artifact:1 [draft] — Zephyr artifact\n"
                   "  <mark>Zephyr</mark> artifact\n"
                   "decision:1 [proposed] — Zephyr decision\n"
                   "  <mark>Zephyr</mark> decision\n");

  // Zero is not "no results" and not "unlimited" — it falls back to the
  // default of 50, which here is every row.
  auto const zero = dispatch(fx, {"search", "zephyr", "--limit", "0", "--json"});
  CHECK(zero.code == 0);
  std::size_t kinds = 0;
  for (std::size_t at = zero.out.find("\"kind\""); at != std::string::npos; at = zero.out.find("\"kind\"", at + 1)) {
    ++kinds;
  }
  CHECK(kinds == 6);
}

TEST_CASE("health hygiene reports a zero-task draft plan across every scope", "[cmd][health][hygiene]") {
  auto const fx = make_fixture("hygiene");
  seed_searchable(fx);
  CHECK(dispatch(fx, {"plan", "create", "Empty draft plan", "--json"}).code == 0);

  auto const text = dispatch(fx, {"health", "hygiene"});
  CHECK(text.code == 0);
  // Plan 1 has a live `todo` task and does NOT appear; plan 2 has none and
  // does. Both halves matter — a port that listed every draft plan would
  // pass any "contains plan 2" assertion.
  CHECK(text.out == "=== Stale draft plans (all tasks terminal) ===\n"
                    "  plan 2: \"Empty draft plan\"\n"
                    "    tasks: 0 todo, 0 doing, 0 blocked, 0 done, 0 cancelled\n"
                    "    suggest: planar plan update 2 --status abandoned  (no tasks ever attached)\n"
                    "\n=== Stale doing tasks (status=doing for >7 days) ===\n"
                    "  none\n"
                    "\n=== Stale open questions (status=open for >30 days) ===\n"
                    "  none\n");

  // No `--scope` means NO scope predicate here, unlike `search`, which
  // resolves the cwd read set and refuses when it is empty. Running from
  // OUTSIDE every registered scope proves it: same report, exit 0.
  auto const outside = dispatch_at(fx, fx.root / "outside", {"health", "hygiene"});
  CHECK(outside.code == 0);
  CHECK(outside.out == text.out);
}

TEST_CASE("health hygiene --json emits parent_plan_id as an explicit null", "[cmd][health][hygiene][json]") {
  auto const fx = make_fixture("hygienejson");
  seed_searchable(fx);
  CHECK(dispatch(fx, {"plan", "create", "Empty draft plan", "--json"}).code == 0);

  auto const json = dispatch(fx, {"health", "hygiene", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out ==
        "{\"thresholds\":{\"stale_doing_days\":7,\"stale_open_days\":30},\"stale_draft_plans\":[{\"id\":2,"
        "\"parent_plan_id\":null,\"title\":\"Empty draft plan\",\"reason\":\"zero_tasks\",\"task_counts\":{\"todo\":0,"
        "\"doing\":0,\"blocked\":0,\"done\":0,\"cancelled\":0},\"suggestion\":\"planar plan update 2 --status "
        "abandoned\"}],\"stale_doing_tasks\":[],\"stale_open_questions\":[]}\n");
}

TEST_CASE("health hygiene refuses three ways and all three are exit 1", "[cmd][health][hygiene][refusal]") {
  auto const fx = make_fixture("hygieneref");
  seed_searchable(fx);

  // Reads like flag validation; is NOT exit 2. See this file's header.
  auto const negative = dispatch(fx, {"health", "hygiene", "--stale-doing", "-1"});
  CHECK(negative.code == 1);
  CHECK(negative.err == "error: stale thresholds must be non-negative\n");

  // `--scope` here must name an ASSOCIATION. A repo ref and `global` are
  // both well-formed scope refs that this verb still rejects.
  auto const repo = dispatch(fx, {"health", "hygiene", "--scope", "repo:proj"});
  CHECK(repo.code == 1);
  CHECK(repo.err == "error: --scope must name one association\n");

  auto const global = dispatch(fx, {"health", "hygiene", "--scope", "global"});
  CHECK(global.code == 1);
  CHECK(global.err == "error: --scope must name one association\n");

  auto const missing = dispatch(fx, {"health", "hygiene", "--scope", "nosuch"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: scope slug not found\n");
}

TEST_CASE("the parent health verb is still an unported refusal", "[cmd][health][unported]") {
  auto const fx = make_fixture("healthparent");
  seed_searchable(fx);

  // `health` is a DUAL node — a working subcommand under an unported
  // parent. Pinned because the failure mode is silent: a dual node that
  // falls through to its help page exits 0, which reads as success.
  auto const parent = dispatch(fx, {"health"});
  CHECK(parent.code == 64);
  CHECK(parent.err == "error: health: not implemented in this build\n");
}

TEST_CASE("audit session renders a bound, active session with padded entries", "[cmd][audit][session]") {
  auto const fx = make_fixture("session");
  seed_searchable(fx);
  CHECK(dispatch(fx, {"capture", "session", "--task", "1"}).code == 0);
  CHECK(dispatch(fx, {"capture", "note", "a second entry"}).code == 0);

  auto const text = dispatch(fx, {"audit", "session", "1"});
  CHECK(text.code == 0);
  // The timestamp is not pinnable, so the header is checked in the two
  // parts that ARE the contract: the bound-task segment and the active
  // marker. The entry lines are pinned WHOLE, because their column padding
  // (`{d:<4}` / `{s:<12}`) is exactly what a reimplementation gets wrong.
  CHECK(text.out.starts_with("session 1  vendor: cli  task:1  "));
  CHECK(text.out.find(" → (active)\n") != std::string::npos);
  CHECK(text.out.ends_with("  1     [action      ]  session opened\n"
                           "  2     [note        ]  a second entry\n"));
}

TEST_CASE("audit session omits unset optionals from --json rather than nulling them", "[cmd][audit][session][json]") {
  auto const fx = make_fixture("sessionjson");
  seed_searchable(fx);
  // No `--task`, so `task_id` is unset; still active, so `ended_at` is too.
  CHECK(dispatch(fx, {"capture", "session"}).code == 0);

  auto const json = dispatch(fx, {"audit", "session", "1", "--json"});
  CHECK(json.code == 0);
  // ABSENT KEYS, not null values — the opposite of `health hygiene --json`
  // in the same cycle.
  CHECK(json.out.find("task_id") == std::string::npos);
  CHECK(json.out.find("ended_at") == std::string::npos);
  CHECK(json.out.find("null") == std::string::npos);
  CHECK(json.out.starts_with("{\"id\":1,\"vendor\":\"cli\",\"started_at\":\""));
  CHECK(json.out.ends_with("\",\"entries\":[{\"ordinal\":1,\"prefix\":\"action\",\"body\":\"session opened\"}]}\n"));

  // Ending the session makes `ended_at` APPEAR, which is what proves the
  // omission above is conditional rather than a field this port forgot.
  CHECK(dispatch(fx, {"capture", "end", "--summary", "done"}).code == 0);
  auto const ended = dispatch(fx, {"audit", "session", "1", "--json"});
  CHECK(ended.code == 0);
  CHECK(ended.out.find("\"ended_at\":\"") != std::string::npos);

  // And the same conditionality for `task_id`, on a session that HAS one.
  //
  // This half was missing from the first draft and a break-probe caught it:
  // mutating the emitter to omit `task_id` unconditionally SURVIVED, because
  // every session this case built was unbound and the absence assertion
  // above passed for the wrong reason. The `ended_at` pair above had its
  // positive half from the start; this one did not.
  //
  // A SECOND fixture, not a second `capture session` in this one: sessions
  // are keyed on the (vendor, vendor_session_id) tuple, so opening another
  // in the same arena reuses row 1 rather than minting a bound row 2.
  auto const bound_fx = make_fixture("sessionjsonbound");
  seed_searchable(bound_fx);
  CHECK(dispatch(bound_fx, {"capture", "session", "--task", "1"}).code == 0);
  auto const bound = dispatch(bound_fx, {"audit", "session", "1", "--json"});
  CHECK(bound.code == 0);
  CHECK(bound.out.find("\"task_id\":1") != std::string::npos);
}

TEST_CASE("audit session gives a non-integer id exit 2 and a missing id exit 1", "[cmd][audit][session][refusal]") {
  auto const fx = make_fixture("sessionbad");
  seed_searchable(fx);

  // One case, two codes, on purpose — see this file's header.
  auto const not_a_number = dispatch(fx, {"audit", "session", "abc"});
  CHECK(not_a_number.code == 2);
  CHECK(not_a_number.err == "error: session id must be an integer, got 'abc'\n");

  auto const missing = dispatch(fx, {"audit", "session", "99"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: session 99 not found\n");

  // `--json` does NOT wrap the refusal in an envelope; the bytes are the
  // same on both streams.
  auto const missing_json = dispatch(fx, {"audit", "session", "99", "--json"});
  CHECK(missing_json.code == 1);
  CHECK(missing_json.err == "error: session 99 not found\n");
  CHECK(missing_json.out.empty());
}

TEST_CASE("the four unported audit leaves still refuse at exit 64", "[cmd][audit][unported]") {
  auto const fx = make_fixture("auditrest");
  seed_searchable(fx);

  // Pinned as a SET so porting one without updating the inventory fails
  // here rather than drifting. Which of the four are blocked on what is in
  // handlers/audit.cppm.
  //
  // The argv differs per leaf and that is NOT incidental: `trail` and
  // `publish-decision` take a positional, `commits` and
  // `handoff-readiness` take none, and handing the latter two a stray
  // argument refuses at exit 2 with `ExtrasError` BEFORE dispatch ever
  // reaches the not-implemented handler. The first draft of this case did
  // exactly that and reported exit 2 for two of the four — a refusal, but
  // the wrong one, and one that would have kept "passing" long after those
  // leaves were ported.
  std::vector<std::pair<std::string, std::vector<std::string>>> const leaves{
      {"trail", {"audit", "trail", "task:1"}},
      {"commits", {"audit", "commits"}},
      {"publish-decision", {"audit", "publish-decision", "1"}},
      {"handoff-readiness", {"audit", "handoff-readiness"}},
  };
  for (auto const& [verb, argv] : leaves) {
    auto const refused = dispatch(fx, argv);
    CHECK(refused.code == 64);
    CHECK(refused.err == std::format("error: audit {}: not implemented in this build\n", verb));
  }
}
