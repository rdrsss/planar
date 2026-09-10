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

TEST_CASE("search renders a rank ABOVE ONE in plain decimal too", "[cmd][search][json]") {
  // THE ASSERTION THE CASE ABOVE COULD NOT MAKE, and the one that would
  // have caught a live defect (task 6261).
  //
  // The deleted `format_zig_float` asked `std::to_chars` for
  // `chars_format::scientific` and then parsed the exponent back with
  // `std::from_chars`. For a NEGATIVE exponent that round-trips (`e-06` ->
  // `-6`) and the fixed-notation shift ran. For a NON-NEGATIVE one
  // `to_chars` writes `e+01`, and `from_chars` REJECTS a leading `+`
  // rather than skipping it -- so the parse failed, the function fell
  // through its own `return std::string{sci}` guard, and RAW SCIENTIFIC
  // reached stdout. Measured on the pre-consolidation installed binary:
  //
  //     "rank":1.4048523469614144e+01     "rank":6.588114215686955e+00
  //
  // Every fixture above searches a term present in EVERY seeded row, where
  // IDF collapses and rank lands near zero with a negative exponent -- the
  // one range the broken path handled correctly. So `find("e-") == npos`
  // passed while the common case, `rank` = `-bm25()` >= 1, was broken.
  // This case seeds a RARE term instead: present in one row out of many,
  // so IDF is large and the rank clears 1.
  auto const fx = make_fixture("rankbig");
  seed_searchable(fx);

  // A CORPUS, then one short row carrying a term no other row has.
  //
  // Both halves are load-bearing, and the first is not obvious. Each entity
  // KIND has its own FTS5 table, so the `N` in bm25's IDF term is the
  // number of rows of THAT KIND -- not the number of rows seeded overall.
  // FTS5 computes `idf = log((N - n + 0.5) / (n + 0.5))` and CLAMPS it to
  // `1e-6` when it comes out <= 0, which is what happens for n = 1 against
  // a handful of rows: with two tasks the idf is `log(1) = 0`, the clamp
  // fires, and the rank arrives as 1.49e-06 -- back in the negative-exponent
  // range this case exists to escape. Twenty filler tasks put N well above
  // n so the idf is real, and the short probe row makes bm25's length
  // normalisation favour it on top of that.
  for (int i = 0; i < 20; ++i) {
    CHECK(dispatch(fx, {"task", "add", std::format("Filler task {}", i), "--plan", "1", "--body",
                        "filler body alpha beta gamma delta epsilon zeta eta theta iota kappa lambda", "--json"})
              .code == 0);
  }
  CHECK(dispatch(fx, {"task", "add", "Quokka", "--plan", "1", "--body", "quokka", "--json"}).code == 0);

  auto const json = dispatch(fx, {"search", "quokka", "--json"});
  CHECK(json.code == 0);

  // THE VACUITY GUARD. An empty hit list would satisfy every absence
  // assertion below.
  REQUIRE(json.out.find("\"rank\":") != std::string::npos);

  // The rank really is at or above 1, so the `e+` assertion is about a
  // value that WOULD have taken the broken path. Without this the case
  // would silently degrade into a second copy of the one above.
  auto const at   = json.out.find("\"rank\":");
  auto const from = at + std::string_view{"\"rank\":"}.size();
  auto const to   = json.out.find_first_of(",}", from);
  REQUIRE(to != std::string::npos);
  auto const rank_text = json.out.substr(from, to - from);
  INFO("rank rendered as: " << rank_text);
  CHECK(std::stod(rank_text) >= 1.0);

  // NEITHER exponent form, in either case. `e+` is the one the old pin
  // could not see; `e-` is kept so a formatter that fixes one by breaking
  // the other cannot pass.
  CHECK(json.out.find("e+") == std::string::npos);
  CHECK(json.out.find("e-") == std::string::npos);
  CHECK(json.out.find("E+") == std::string::npos);

  // The rendered digits are plain decimal, so the text a consumer parses
  // is the text a human reads.
  CHECK(rank_text.find_first_not_of("0123456789.") == std::string::npos);
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

TEST_CASE("the parent health verb reports ok on a fresh database with no managed install", "[cmd][health]") {
  // `health` is a DUAL node — a real handler alongside its `hygiene`
  // subcommand, ported at task 6357. The fixture's HOME (`root/fakehome`)
  // never exists, so this exercises the "no install manifest at all" arm
  // hermetically — no real `$HOME`/`$PLANAR_HOME` is ever consulted (see
  // this file's header note on `make_fixture`).
  auto const fx = make_fixture("healthparent");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  auto const parent = dispatch(fx, {"health"});
  CHECK(parent.code == 0);
  CHECK(parent.err.empty());
  CHECK(parent.out.contains("overall:          ok"));
  CHECK(parent.out.contains("projection freshness: not_installed"));
  CHECK(parent.out.contains("projection manifest:  missing"));
  // `with_projection_freshness` forwards the classifier's OWN reason string
  // as evidence whenever one exists, regardless of whether the manifest
  // state is itself degraded — a missing manifest is `not_installed`, NOT
  // `degraded`, but it still carries a reason ("install manifest is
  // missing"), so evidence IS present here. Oracle-confirmed: `evidence`
  // is null only when `status.reason` is unset AND nothing is
  // managed-degraded either. `repair_command`, by contrast, is forwarded
  // ONLY when `degraded`, so it stays absent here even though the
  // classifier itself always names a bootstrap command.
  CHECK(parent.out.contains("projection evidence:  install manifest is missing"));
  CHECK_FALSE(parent.out.contains("projection repair:"));
}

TEST_CASE("planar health --json emits the full field set with explicit nulls", "[cmd][health][json]") {
  auto const fx = make_fixture("healthjson");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  auto const json = dispatch(fx, {"health", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.contains("\"db_ok\":true"));
  CHECK(json.out.contains("\"overall\":\"ok\""));
  CHECK(json.out.contains("\"manifest_status\":\"missing\""));
  // `evidence` carries the classifier's reason string even in this
  // NOT-degraded arm (see the text-mode case's note above); only
  // `repair_command` stays an explicit JSON null here.
  CHECK(json.out.contains("\"evidence\":\"install manifest is missing\""));
  CHECK(json.out.contains("\"repair_command\":null"));
}

TEST_CASE("planar health refuses when HOME is unset, without ever opening a real home", "[cmd][health][refusal]") {
  // Iteration-3 review: an exhaustive permissive-mutation sweep found this
  // refusal (`resolve_homes`'s `HomeNotSet` arm) had NO fixture in either
  // direction — every other `[cmd][health]` case runs through `make_fixture`,
  // which always sets HOME. Constructed directly here (rather than through
  // `dispatch`/`make_fixture`) specifically so `HOME` can be OMITTED from the
  // environment map entirely, never merely emptied.
  auto const fx = make_fixture("healthnohome");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  std::vector<std::string> argv{"planar", "health"};
  std::ostringstream       out;
  std::ostringstream       err;
  planar::cmd::context ctx{argv, planar::cmd::map_env({{"PLANAR_DB", fx.db_path.string()}}), fx.root / "proj", fx.db_path, out,
                           err};
  auto const           tree  = planar::cmd::root_app();
  auto const           table = planar::cmd::handlers(*tree);
  int const            code  = planar::cmd::run(ctx, *tree, table);
  CHECK(code == 1);
  CHECK(err.str() == "error: health check failed: resolving install homes: HomeNotSet\n");
  CHECK(out.str().empty());
}

TEST_CASE("planar health refuses when HOME is set but empty, distinct from HOME being absent", "[cmd][health][refusal]") {
  // Isolates `resolve_homes`'s `home->empty()` clause from `!home.has_value()`
  // above — an explicit empty string is a DIFFERENT environment shape than
  // an omitted key, and a permissive mutation of only this clause survived
  // the sibling test above (which never sets the key at all).
  auto const fx = make_fixture("healthemptyhome");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  std::vector<std::string> argv{"planar", "health"};
  std::ostringstream       out;
  std::ostringstream       err;
  planar::cmd::context     ctx{
      argv, planar::cmd::map_env({{"PLANAR_DB", fx.db_path.string()}, {"HOME", ""}}), fx.root / "proj", fx.db_path, out, err};
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);
  int const  code  = planar::cmd::run(ctx, *tree, table);
  CHECK(code == 1);
  CHECK(err.str() == "error: health check failed: resolving install homes: HomeNotSet\n");
  CHECK(out.str().empty());
}

TEST_CASE("planar health exits 1 and reports degraded for an unresumable doing task", "[cmd][health][degraded]") {
  auto const fx = make_fixture("healthdegraded");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "A plan", "--scope", "global", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "A task", "--plan", "1", "--json"}).code == 0);
  // No --next-action, so this task is in-flight but NOT resumable — the
  // exact contributor `check()` flags.
  CHECK(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);

  auto const degraded = dispatch(fx, {"health"});
  CHECK(degraded.code == 1);
  // The report was still written to stdout, and NOTHING extra reached
  // stderr — the oracle's own exit(1)-after-output contract, mirrored via
  // an EMPTY rendered stderr payload rather than an "error: " line. See
  // handlers/health.cppm's header for why this is not an ordinary failure.
  CHECK(degraded.err.empty());
  CHECK(degraded.out.contains("in-flight tasks:  1 (0 resumable, 1 NOT resumable)"));
  CHECK(degraded.out.contains("overall:          degraded"));
}

TEST_CASE("planar health --json also reports degraded and empty stderr", "[cmd][health][degraded][json]") {
  auto const fx = make_fixture("healthdegradedjson");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "A plan", "--scope", "global", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "A task", "--plan", "1", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);

  auto const degraded = dispatch(fx, {"health", "--json"});
  CHECK(degraded.code == 1);
  CHECK(degraded.err.empty());
  CHECK(degraded.out.contains("\"not_resumable_tasks\":1"));
  CHECK(degraded.out.contains("\"overall\":\"degraded\""));
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

// This file used to carry "the two unported audit leaves still refuse at
// exit 64" here, pinning `audit publish-decision` and (earlier still)
// `audit handoff-readiness`, `audit commits` and `audit trail` as each was
// added and then removed from the set. `audit publish-decision` was the
// family's last remaining entry — removed at task 6339 once `postComment`
// landed on both adapters — so the set this case pinned is now EMPTY and
// the case itself is gone rather than kept with nothing to assert. Its argv
// moves to audit_publish_decision_leaf.t.cpp, where `audit publish-decision
// 1` now pins real behaviour (the scope guard, the fixture-server request
// log, the unconditional re-post) instead of an exit-64 placeholder. The
// `audit` family is now fully ported and contributes nothing to this file.
