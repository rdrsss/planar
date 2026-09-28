// @file scope_leaves.t.cpp
// @brief In-process tests for the five `planar scope` leaves wired by plan
// 996, task 6214: `show`, `suggest`, `use`, `pop`, `clear`.
//
// ## THE TWO WORKING LEAVES DISAGREE ABOUT WHAT "THE CWD PROJECT" MEANS,
// ## AND THAT IS THE CENTRAL CASE HERE
//
// `scope show` resolves by LONGEST PATH PREFIX, so it answers from any
// directory at or below a registered project root. `scope suggest` queries
// `projects where root_path = ?` — an EXACT match — so it answers only AT
// the root and reports "no scope suggestions for cwd" one directory down.
//
// Both were captured from `zig/zig-out/bin/planar` at the SAME two cwds in
// the SAME arena, and the pair is asserted together in one case rather than
// separately, because that is the only shape that fails if someone
// "harmonises" them. A subdirectory that suggests nothing while showing a
// scope reads like a bug until you know it is the contract; the fix that
// looks obvious (widen `suggest` to a prefix match) is a behaviour change
// this port is not entitled to make.
//
// ## `scope suggest --json` IS NDJSON, INCLUDING WHEN EMPTY (task 6257)
//
// Populated, it emits ONE JSON OBJECT PER LINE with no wrapper:
//
//     {"slug":"demo","association_id":1,"reason":"explicit member"}
//     {"slug":"other","association_id":2,"reason":"explicit member"}
//
// Empty, it emits NOTHING — zero bytes, exit 0. That is the NDJSON
// contract: N lines for N results, and N may be zero.
//
// It did not used to. Empty emitted a single `{"proposals":[]}` object
// under a key no populated invocation ever produced, so a consumer written
// against either shape broke on the other — and the empty case is the one
// people write their parser against first, because it is the easy fixture.
// Decision 1090 authorises this pin rewrite, applying to task 6257 the
// reasoning decision 1067 applied to its own nine rows: with the oracle
// deleted there is nothing left to reproduce bug-for-bug against, so D2's
// rule no longer decides defects with real consequences. 6257 is one, and the rule chosen for the whole
// shape-split family (6257 / 6270 / 6326) is NDJSON-with-zero-lines,
// because it leaves the POPULATED shape — the one field consumers actually
// read — byte-identical, and it is what `links list --json` already did.
//
// ## AN UNKNOWN `--scope` WRITES **TWO** STDERR LINES
//
//     error: resolving scope: scope slug not found: nosuchscope
//     error: SlugNotFound
//
// exit 1. The first names the slug the operator typed and is written by
// `resolveForReadSet` itself; the second is the bare Zig tag main reports
// after the error propagates. Emitting only one of the two is the easy
// mistake in both directions — drop the first and the operator cannot see
// WHICH slug was wrong, drop the second and every script grepping the tag
// breaks — so both lines are asserted as one exact payload.
//
// `--scope ""` takes this SAME path, with the slug interpolated as nothing.
// That is worth its own case: the sibling `--status ""` flag on five other
// families comma-splits to zero tokens and never reaches its validator at
// all, which has already produced one withdrawn defect report on this
// milestone. `--scope` is not comma-split, so the empty value DOES reach
// the resolver and DOES refuse. Captured, not assumed.
//
// ## THE THREE REMOVAL STUBS ARE REFUSALS EITHER WAY, SO THE BYTES ARE THE
// ## ONLY CONTRACT
//
// An unported leaf refuses at exit 64 with "not implemented in this build".
// These three refuse at exit 2 with a paragraph naming plan 153 M5 and
// three concrete remedies. Nothing about "it refused" distinguishes the
// two, so each stub asserts its FULL stderr payload and its exit code, and
// each asserts `db_opened() == false` — the oracle's `removed.zig` never
// calls `ensureDb`, so a port that opened SQLite to refuse would create and
// migrate a database on a pure error path.
//
// ## HOME / DB SAFETY
//
// Every fixture builds an explicit environment map rooted at its own
// scratch directory — `PLANAR_DB`, `PLANAR_HOME`, `PLANAR_LOCAL_HOME` and
// `HOME` all point inside it, and nothing here reads the process
// environment. `scope show` and `scope suggest` DO open SQLite (unlike the
// three stubs), so the pinned `PLANAR_DB` is load-bearing rather than
// decorative: without it these cases would migrate the operator's live
// `~/.planar/planar.db`.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string below was captured by running
// `zig/zig-out/bin/planar` and this binary over a 37-step argv sequence in
// two pinned arenas sharing ONE working directory (the shared cwd matters:
// `scope show --json` echoes the cwd into its payload and `scope suggest`
// keys on it, so two arenas at different paths would diverge for reasons
// unrelated to the port). Streams were captured through a PIPE, never
// `2> file` — the Zig writer uses positional writes and a second write to
// a seekable file lands at offset 0 and eats the first.
//
// All 37 steps agree apart from two classes of oracle-side noise, neither
// of which is a behavioural difference:
//
//   - Three payloads carry the arena path or a wall-clock timestamp
//     (`init`, and the two `assoc create`s).
//   - The four unknown-`--scope` steps: the oracle appends a TWENTY-FOUR
//     LINE Zig error-return stack trace, with absolute source paths and hex
//     addresses, AFTER the two message lines. The two message lines
//     themselves are byte-identical (verified by diffing this binary's
//     whole stderr against the oracle's first two lines). The trace is a
//     ReleaseSafe artifact of `return e` propagating to `main`, it is NOT
//     operator-facing output any consumer should see, and this tree has no
//     equivalent by construction. It is reported as an oracle defect rather
//     than reproduced.
//
//     Worth knowing if you re-run this: the trace is deterministic but
//     appears only once the Zig build's debug info is resolvable. An
//     earlier sweep in this same cycle saw the oracle emit the two lines
//     ALONE and reported a clean match; a later `zig build` made the trace
//     appear. "The oracle agreed last time" is therefore not by itself
//     evidence about this path.

#include <catch2/catch_test_macros.hpp>
// Glaze is this binary's JSON reader (`planar.json_text` is emit-only).
// Task 6254's escaping assertions are paired with a real PARSE, because a
// substring check passes on the malformed form too — which is how the
// unescaped interpolation survived this file's other pins.
#include <glaze/glaze.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;
import planar.engine.identity;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Build a fixture under a unique scratch directory, with `proj/sub`
/// already present so the prefix-vs-exact cases have somewhere to stand.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_scope_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj" / "sub", ec);
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

/// @brief Dispatch `args` against the real tree and table, from `cwd`.
/// @param fx The fixture.
/// @param cwd The working directory to resolve scope from.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch_at(const fixture& fx, const std::filesystem::path& cwd, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  auto vars   = fx.vars;
  vars["PWD"] = cwd.string();

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{
      std::move(argv), planar::cmd::map_env(vars), cwd, std::make_shared<planar::cmd::database>(fx.db_path, err), out, err};
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::make_handler_table(*tree);
  int const  code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Dispatch from the fixture's project root.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  return dispatch_at(fx, fx.root / "proj", std::move(args));
}

/// @brief The trailer every non-JSON `scope show` arm ends with.
constexpr std::string_view k_trailer =
    "\n(The active scope stack was removed in plan 153 M5; scope is now derived from your current working directory.)\n";

/// @brief Register the project and bind it to `slug`.
///
/// The path is passed ABSOLUTE and that is deliberate. `assoc add demo .`
/// stores the literal string `.` in `projects.root_path` — a row no
/// cwd-derivation can ever match again — so a fixture built with `.` leaves
/// every interesting arm of both verbs unreachable and every case below
/// passes VACUOUSLY on the empty answer. That is exactly what the first
/// draft of this fixture did, and the oracle agreed with it, which is why
/// the mistake is recorded here rather than merely avoided.
/// @param fx The fixture.
/// @param slug The association slug.
/// @param kind The association kind.
void bind_project(const fixture& fx, std::string_view slug, std::string_view kind) {
  CHECK(dispatch(fx, {"assoc", "create", std::string{slug}, "--kind", std::string{kind}, "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "add", std::string{slug}, (fx.root / "proj").string()}).code == 0);
}

} // namespace

TEST_CASE("an unbound cwd shows no scope and suggests nothing, in four shapes", "[cmd][scope][show][suggest]") {
  auto const fx = make_fixture("virgin");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  auto const text = dispatch(fx, {"scope", "show"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out == std::format("resolved scope: none (cwd not inside any registered Planar scope)\n\n"
                                "cd into a registered scope or pass --scope <slug> to any verb.\n{}",
                                k_trailer));

  // `"resolved_scopes":[]` AND `"source":"none"` — the empty array alone
  // would also be produced by a `--scope` that matched nothing, which
  // reports `"flag"`. The pair is the contract.
  auto const json = dispatch(fx, {"scope", "show", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == std::format(R"({{"resolved_scopes":[],"source":"none","cwd":"{}"}})"
                                "\n",
                                (fx.root / "proj").string()));

  auto const stext = dispatch(fx, {"scope", "suggest"});
  CHECK(stext.code == 0);
  CHECK(stext.out == "no scope suggestions for cwd\n");

  // Zero bytes, exit 0 — the NDJSON empty case (task 6257). NOT the old
  // `{"proposals":[]}` object, and not `[]`: the populated form is a
  // stream of bare objects and an empty stream has no lines in it.
  auto const sjson = dispatch(fx, {"scope", "suggest", "--json"});
  CHECK(sjson.code == 0);
  CHECK(sjson.out.empty());
  // ...while the TEXT form (asserted above) still says so in words. That
  // is where an operator, as opposed to a parser, learns it was empty.
}

TEST_CASE("show matches by PREFIX and suggest matches EXACTLY, from the same two cwds", "[cmd][scope][show][suggest][cwd]") {
  auto const fx = make_fixture("prefixvsexact");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, "demo", "project");

  auto const root = fx.root / "proj";
  auto const sub  = fx.root / "proj" / "sub";

  // AT the root: both verbs answer.
  auto const show_root = dispatch_at(fx, root, {"scope", "show"});
  CHECK(show_root.code == 0);
  CHECK(show_root.out == std::format("resolved scope (from cwd):\n  project:demo\n{}", k_trailer));
  auto const sugg_root = dispatch_at(fx, root, {"scope", "suggest"});
  CHECK(sugg_root.code == 0);
  CHECK(sugg_root.out == "suggested scope based on cwd:\n  demo  (explicit member)\n");

  // ONE DIRECTORY DOWN: `show` still resolves, `suggest` goes empty. This
  // is the divergence; asserting the pair together is what makes a later
  // "harmonisation" of the two matchers fail here.
  auto const show_sub = dispatch_at(fx, sub, {"scope", "show"});
  CHECK(show_sub.code == 0);
  CHECK(show_sub.out == std::format("resolved scope (from cwd):\n  project:demo\n{}", k_trailer));
  auto const sugg_sub = dispatch_at(fx, sub, {"scope", "suggest"});
  CHECK(sugg_sub.code == 0);
  CHECK(sugg_sub.out == "no scope suggestions for cwd\n");

  // ...and the JSON forms carry the SAME split, with the cwd echoed
  // per-invocation rather than pinned to the project root.
  auto const show_sub_json = dispatch_at(fx, sub, {"scope", "show", "--json"});
  CHECK(
      show_sub_json.out ==
      std::format(
          R"({{"resolved_scopes":[{{"kind":"association","id":1,"slug":"demo","name":"demo","kind_label":"project"}}],"source":"cwd","cwd":"{}"}})"
          "\n",
          sub.string()));
  CHECK(dispatch_at(fx, sub, {"scope", "suggest", "--json"}).out.empty()); // task 6257

  // OUTSIDE the project entirely: neither answers, which is what proves
  // the prefix match above was a match rather than an unconditional hit.
  auto const outside = dispatch_at(fx, fx.root / "outside", {"scope", "show"});
  CHECK(outside.code == 0);
  CHECK(outside.out.starts_with("resolved scope: none"));
}

// NOTE — a TEST_CASE title must not START with `--`. `catch_discover_tests`
// registers the title as the ctest NAME and hands it back to the binary as
// an argument, where Catch2's own CLI parses a leading `--` as a flag and
// dies with "Unrecognised token" before running anything. The failure is
// invisible when the case is run by TAG (it passes) and appears only under
// `ctest`, which is how the first draft of this file shipped two silently
// broken cases. Both were renamed to lead with the flag's NAME instead.
TEST_CASE("the --scope flag re-labels the heading and flips `source` to flag, even from outside",
          "[cmd][scope][show][override]") {
  auto const fx = make_fixture("override");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, "demo", "project");

  // The text form records provenance ONLY in the heading — the row below
  // it is byte-identical to the cwd-derived one, so the heading is the
  // whole difference.
  auto const flagged = dispatch(fx, {"scope", "show", "--scope", "demo"});
  CHECK(flagged.code == 0);
  CHECK(flagged.out == std::format("resolved scope (from --scope flag):\n  project:demo\n{}", k_trailer));

  // From OUTSIDE any registered scope the flag still resolves, which is
  // the point of the flag and also proves the read set came from the flag
  // rather than from the cwd: the same cwd with no flag answers "none".
  auto const outside_flagged = dispatch_at(fx, fx.root / "outside", {"scope", "show", "--scope", "demo", "--json"});
  CHECK(outside_flagged.code == 0);
  CHECK(
      outside_flagged.out ==
      std::format(
          R"({{"resolved_scopes":[{{"kind":"association","id":1,"slug":"demo","name":"demo","kind_label":"project"}}],"source":"flag","cwd":"{}"}})"
          "\n",
          (fx.root / "outside").string()));
  CHECK(dispatch_at(fx, fx.root / "outside", {"scope", "show", "--json"}).out.find(R"("source":"none")") != std::string::npos);
}

TEST_CASE("an unknown --scope writes BOTH stderr lines and exits 1", "[cmd][scope][show][refusal]") {
  auto const fx = make_fixture("badscope");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, "demo", "project");

  for (auto const* json : {static_cast<const char*>(nullptr), "--json"}) {
    std::vector<std::string> args{"scope", "show", "--scope", "nosuchscope"};
    if (json != nullptr) {
      args.emplace_back(json);
    }
    INFO("json arm: " << (json == nullptr ? "text" : json));
    auto const got = dispatch(fx, args);
    CHECK(got.code == 1);
    // NOTHING PARTIAL on stdout — not a partial payload, not an empty
    // `resolved_scopes` array. The JSON arm refuses BEFORE it writes its
    // opening brace, and a port that resolved the set after emitting the
    // prefix would leave truncated JSON behind on this path. Under
    // `--json` (decision 1145, task 6844) the ADDITIVE error envelope is
    // the only thing on stdout; the text arm still writes nothing there.
    if (json != nullptr) {
      CHECK(got.out == planar::cmd::testsupport::json_error_envelope_line("scope show", "generic_failure"));
    } else {
      CHECK(got.out.empty());
    }
    CHECK(got.err == "error: resolving scope: scope slug not found: nosuchscope\nerror: SlugNotFound\n");
  }
}

TEST_CASE("an EMPTY --scope reaches the resolver and refuses, unlike the comma-split --status flags",
          "[cmd][scope][show][refusal][empty-flag]") {
  auto const fx = make_fixture("emptyscope");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, "demo", "project");

  auto const got = dispatch(fx, {"scope", "show", "--scope", ""});
  // Exit 1 and NOT 0: the empty string is a slug that does not exist, not
  // an absent flag. Were it collapsed to `nullopt` this would silently
  // succeed with the cwd-derived answer, which is the same "optional
  // argument quietly taking its default" shape that has produced real
  // defects elsewhere in this port.
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  // The slug interpolates as NOTHING, leaving a trailing space before the
  // newline. Pinned exactly, trailing space included.
  CHECK(got.err == "error: resolving scope: scope slug not found: \nerror: SlugNotFound\n");
}

TEST_CASE("suggest lists EVERY membership, one JSON object per line", "[cmd][scope][suggest][json]") {
  auto const fx = make_fixture("multi");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, "demo", "project");
  bind_project(fx, "other", "client");

  // Ordered by association SLUG, not by id and not by insertion — `demo`
  // before `other` happens to agree with both here, so the ordering is
  // asserted rather than inferred from this fixture alone (see the engine
  // test, which seeds slugs whose alphabetical and id orders disagree).
  auto const text = dispatch(fx, {"scope", "suggest"});
  CHECK(text.code == 0);
  CHECK(text.out == "suggested scope based on cwd:\n  demo  (explicit member)\n  other  (explicit member)\n");

  // NDJSON with NO wrapper. UNCHANGED by task 6257 — the fix moved the
  // empty case onto this shape, it did not move this case onto a wrapper.
  // Pinning the populated bytes here is what proves that.
  auto const json = dispatch(fx, {"scope", "suggest", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "{\"slug\":\"demo\",\"association_id\":1,\"reason\":\"explicit member\"}\n"
                    "{\"slug\":\"other\",\"association_id\":2,\"reason\":\"explicit member\"}\n");

  // `scope show` picks ONE of the two by specificity rank (project beats
  // client), so the two verbs answer differently on the same cwd. Pinned
  // so a change to the ranking surfaces here rather than in a listing verb.
  CHECK(dispatch(fx, {"scope", "show"}).out == std::format("resolved scope (from cwd):\n  project:demo\n{}", k_trailer));
}

TEST_CASE("use / pop / clear refuse at exit 2 WITHOUT opening SQLite", "[cmd][scope][removed]") {
  auto const fx = make_fixture("removed");

  for (auto const& verb : {"use", "pop", "clear"}) {
    INFO("removed leaf: scope " << verb);
    auto const got = dispatch(fx, {"scope", std::string{verb}});
    // Exit 2, NOT 64. An unported leaf answers 64 with "not implemented in
    // this build" — a promise that a later build will implement it. These
    // three will not; the verb is gone.
    CHECK(got.code == 2);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: `planar scope {}` was removed in plan 153 M5; the active scope stack is gone. "
                                 "Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar "
                                 "scope show` to inspect the cwd-derived scope.\n",
                                 verb));
    // The lazy-database rule on a pure refusal path: `removed.zig` never
    // calls `ensureDb`, so refusing must not create and migrate a database.
    CHECK_FALSE(got.db_open);
  }

  // `scope use` takes an OPTIONAL positional, and supplying it changes
  // nothing — same bytes, same code. A port that echoed the slug back, or
  // that refused differently when one was given, would break here.
  auto const with_arg = dispatch(fx, {"scope", "use", "somescope"});
  CHECK(with_arg.code == 2);
  CHECK(with_arg.err == dispatch(fx, {"scope", "use"}).err);
  CHECK_FALSE(with_arg.db_open);

  // ...and nothing above created the database file at all.
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
}

// Task 6664, closing the gate blind spot task 6661 found: `make
// surface-check` hashes only the schema catalog and every `--help` page, and
// `allow_extras` (cliapp/surface.cppm:98) appears in NEITHER — the catalog
// emitter never writes it and no help renderer prints it. Task 6635's Probe C
// deleted all three `set_allow_extras` calls in `declare_scope` and the gate
// stayed clean at 312 points with every existing Catch2 case (including the
// one just above) still passing, because that case never hands the three
// verbs a token CLI11 itself would otherwise reject. THIS case does: an
// unrecognized flag is exactly the shape `set_allow_extras` exists to
// swallow before the handler's own `removed_in_m5` refusal ever gets a
// chance to run.
//
//     with allow_extras:    the plan-153-M5 removal paragraph (below)
//     without allow_extras: `error: <verb>: The following arguments were
//                           not expected: --stack-name bar` (CLI11's own
//                           message, not the handler's)
//
// Both refuse at exit 2, which is precisely why exit status cannot stand in
// for the message: this case asserts the byte-for-byte paragraph, not just
// "it failed".
TEST_CASE("use / pop / clear swallow an unrecognized flag into the SAME removal paragraph",
          "[cmd][scope][removed][allow_extras]") {
  auto const fx = make_fixture("removed-extras");

  for (auto const& verb : {"use", "pop", "clear"}) {
    INFO("allow_extras leaf: scope " << verb);
    auto const got = dispatch(fx, {"scope", std::string{verb}, "somescope", "--stack-name", "bar"});
    CHECK(got.code == 2);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: `planar scope {}` was removed in plan 153 M5; the active scope stack is gone. "
                                 "Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar "
                                 "scope show` to inspect the cwd-derived scope.\n",
                                 verb));
    CHECK_FALSE(got.db_open);
  }

  CHECK_FALSE(std::filesystem::exists(fx.db_path));
}

TEST_CASE("reason_from_source maps the four known sources and passes anything else through", "[engine][scope][suggest][reason]") {
  namespace id = planar::engine::identity;
  // Literal expectations on both sides. Deriving either side from the
  // other — or from the function under test — is the shape that stays
  // green through a break-probe.
  CHECK(id::reason_from_source("user") == "explicit member");
  CHECK(id::reason_from_source("auto:git-remote") == "from git remote");
  CHECK(id::reason_from_source("auto:path") == "from parent directory");
  CHECK(id::reason_from_source("auto:lang") == "from language ecosystem");
  // The fallthrough returns the RAW source, not a placeholder: a value
  // written by a newer binary degrades to something readable rather than
  // to `unknown`.
  CHECK(id::reason_from_source("auto:something-new") == "auto:something-new");
  CHECK(id::reason_from_source("") == "");
}

// =========================================================================
// Escaping (task 6254)
// =========================================================================

TEST_CASE("scope show/suggest --json ESCAPE cwd, slug, name and reason", "[cmd][scope][json][escape]") {
  // Task 6254. All three verbs' `--json` arms interpolated their strings
  // through a raw `std::format`, so a quote or a backslash anywhere in a
  // cwd or an association slug emitted a document no parser reads.
  // Reproduced against build/debug/bin/planar under a scratch HOME +
  // PLANAR_DB before the fix:
  //
  //   $ planar scope show --scope 'ev"il' --json
  //   {"resolved_scopes":[{...,"slug":"ev"il","name":"ev"il",...}],...}
  //   $ cd 'we"ird' && planar scope show --json
  //   {...,"cwd":"/…/we"ird"}
  //   $ planar scope suggest --json
  //   {"slug":"ev"il","association_id":1,"reason":"explicit member"}
  //
  // A BACKSLASH is the byte that reaches this by accident rather than by
  // malice: it is legal in a POSIX path component and it is every
  // separator on Windows, where the body of 6254 notes `scope show --json`
  // would be broken by default rather than as an edge case.
  auto const fx = make_fixture("escape");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  bind_project(fx, R"(ev"il)", "project");

  // 1. `slug` and `name` on the association arm of `json_row`, reached via
  //    `--scope` so the specificity contest cannot pick the repo instead.
  auto const flagged = dispatch(fx, {"scope", "show", "--scope", R"(ev"il)", "--json"});
  CHECK(flagged.code == 0);
  CHECK(flagged.out.find(R"("slug":"ev\"il","name":"ev\"il")") != std::string::npos);
  CHECK(flagged.out.find(R"("slug":"ev"il")") == std::string::npos);

  // 2. `cwd`, from a directory whose own name carries both bytes.
  auto const      weird = fx.root / R"(we"ird\path)";
  std::error_code ec;
  std::filesystem::create_directories(weird, ec);
  REQUIRE_FALSE(ec);
  auto const shown = dispatch_at(fx, weird, {"scope", "show", "--json"});
  CHECK(shown.code == 0);
  CHECK(shown.out.find(R"(we\"ird\\path)") != std::string::npos);

  // 3. `slug` and `reason` on the NDJSON suggest line.
  auto const sugg = dispatch(fx, {"scope", "suggest", "--json"});
  CHECK(sugg.code == 0);
  CHECK(sugg.out == R"({"slug":"ev\"il","association_id":1,"reason":"explicit member"})"
                    "\n");

  // 4. The whole `scope show` envelope parses. A substring check would
  //    have passed on the malformed form too, which is how this survived.
  CHECK(glz::validate_json(flagged.out).ec == glz::error_code::none);
  CHECK(glz::validate_json(shown.out).ec == glz::error_code::none);
  CHECK(glz::validate_json(sugg.out).ec == glz::error_code::none);

  // The TEXT forms are unchanged; they are not JSON and never escaped.
  CHECK(dispatch(fx, {"scope", "suggest"}).out == "suggested scope based on cwd:\n  ev\"il  (explicit member)\n");
}

TEST_CASE("scope show --json escapes the REPO arm's slug", "[cmd][scope][json][escape]") {
  // THE THIRD ARM of `json_row`. The case above reaches the ASSOCIATION
  // arm (via `--scope`, so the specificity contest cannot pick the repo)
  // and the `cwd` tail. It never renders a `"kind":"repo"` row, so a
  // break-probe that reverted ONLY the repo arm to raw `std::format`
  // interpolation SURVIVED it -- the whole suite stayed green with the
  // defect back in place. This case closes that hole.
  //
  // Repo rows are not hypothetical: `resolve_read_scope_set` returns one
  // whenever the cwd is inside a registered project but not at an
  // association root, and its `slug` is `projects.slug` -- operator-
  // supplied through `init --slug`, so every byte the operator types
  // reaches the renderer.
  auto const fx = make_fixture("escaperepo");
  CHECK(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  // A project, and NO association bound to it, so the read set resolves to
  // the repo row alone rather than letting an association outrank it.
  auto const      repo = fx.root / "proj";
  std::error_code ec;
  std::filesystem::create_directories(repo, ec);
  REQUIRE_FALSE(ec);
  auto const registered = dispatch_at(fx, repo, {"init", "--slug", R"(ev"il\repo)", "--allow-no-repo", "--force", "--json"});
  CHECK(registered.code == 0);

  auto const shown = dispatch_at(fx, repo, {"scope", "show", "--json"});
  CHECK(shown.code == 0);

  // THE VACUITY GUARD. Without a repo row present, every assertion below
  // is about a document that does not contain the arm under test.
  REQUIRE(shown.out.find(R"("kind":"repo")") != std::string::npos);

  // Both bytes escaped, and the raw form absent. The raw-form check is the
  // one that fails under the reverted arm; a bare substring search for the
  // escaped form would also match inside the malformed document.
  CHECK(shown.out.find(R"("slug":"ev\"il\\repo")") != std::string::npos);
  CHECK(shown.out.find(R"("slug":"ev"il)") == std::string::npos);

  // And the envelope PARSES, which is the contract `--json` actually makes.
  CHECK(glz::validate_json(shown.out).ec == glz::error_code::none);
}
