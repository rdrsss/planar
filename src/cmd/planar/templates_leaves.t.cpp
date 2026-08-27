// @file templates_leaves.t.cpp
// @brief In-process tests for the six `planar templates` leaves wired by
// plan 996, task 6190.
//
// ## HOME / DB SAFETY
//
// This family WRITES FILES (`templates init` extracts ten of them), so a
// wrong root would edit the developer's real `~/.planar/templates`. Every
// fixture builds an explicit environment map with `HOME` and
// `PLANAR_CONFIG_PATH` under its own uniquely-named scratch root and
// dispatches through `map_env`, so nothing here reads the process
// environment.
//
// The variable that matters is `HOME`, NOT `PLANAR_HOME`: the templates
// root is `templates.dir` from the resolved config (default
// `~/.planar/templates`), and the `~` expands against `$HOME`. Oracle-
// confirmed under a pinned arena where the two pointed at different
// directories — `templates path` printed the one under `HOME`. Redirecting
// only `PLANAR_HOME`, the obvious move, would leave `templates init`
// writing into the real home.
//
// ## FIVE OF THE SIX MUST NOT OPEN SQLITE
//
// Only `templates render` needs a database. The other five are asserted to
// leave `ctx.db_opened()` FALSE, which is both a real contract — listing
// what the binary ships cannot require the operator to have run
// `planar init` — and a structural guarantee that this file cannot reach
// the operator's `planar.db`.
//
// ## THE FOUR EMPTY-OUTPUT SPELLINGS
//
// `list`/`init` each disagree with themselves between text and `--json`,
// and none of the four is derivable from another. All four are captured
// from the built Zig oracle and pinned here:
//
//   templates list        (no matches)  -> `no templates found\n`
//   templates list --json (no matches)  -> ZERO BYTES
//   templates init        (all present) -> `templates init: nothing to do (all templates already present)\n`
//   templates init --json (all present) -> ZERO BYTES
//
// ## FILTERS ARE PROVEN TO EXCLUDE, AND EMPTY IS NOT ABSENT
//
// `templates list --system ""` lists EVERYTHING — an empty flag value means
// "no filter", not "match the empty string". That is the empty-value hazard
// that produced a withdrawn false-positive defect report elsewhere in this
// milestone, so it is pinned directly rather than assumed. The non-empty
// filter cases assert both halves: the matching row survives AND the
// non-matching row is gone.

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
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Created only by the `render` cases.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_tmplleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome", ec);
  return fixture{
      .root = root,
      // NOTE: `PLANAR_HOME` is deliberately absent. This family never reads
      // it, and leaving it unset proves the tests do not depend on it.
      .vars    = {{"HOME", (root / "fakehome").string()},
                  {"PLANAR_CONFIG_PATH", (root / "config.toml").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Remove a fixture's scratch tree.
/// @param fx The fixture.
auto cleanup(const fixture& fx) -> void {
  std::error_code ec;
  std::filesystem::remove_all(fx.root, ec);
}

/// @brief The templates root a fixture resolves to.
/// @param fx The fixture.
/// @return `<root>/fakehome/.planar/templates`.
auto templates_root(const fixture& fx) -> std::filesystem::path {
  return fx.root / "fakehome" / ".planar" / "templates";
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

/// @brief Write a file, creating its parent directories.
/// @param path Where.
/// @param body What.
auto write_file(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << body;
}

/// @brief How many lines `text` has.
/// @param text The text.
/// @return The count of '\n'.
auto line_count(std::string_view text) -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

} // namespace

// --- templates path ---------------------------------------------------

TEST_CASE("templates path prints the HOME-derived root and opens no database", "[cmd][templates][path]") {
  auto const fx = make_fixture("path");
  auto const r  = dispatch(fx, {"templates", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == templates_root(fx).string() + "\n");
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

TEST_CASE("templates path IGNORES --system, --set and --json", "[cmd][templates][path][oracle-defect]") {
  // All three are declared in the CLI tree and none is used — the zig
  // handler discards them explicitly. Reproduced rather than implemented;
  // a caller must not assume they do anything. Oracle-captured:
  // `templates path --json` printed the bare path, not an envelope.
  auto const fx       = make_fixture("pathflags");
  auto const expected = templates_root(fx).string() + "\n";
  CHECK(dispatch(fx, {"templates", "path", "--json"}).out == expected);
  CHECK(dispatch(fx, {"templates", "path", "--system", "jira"}).out == expected);
  CHECK(dispatch(fx, {"templates", "path", "--set", "probe"}).out == expected);
  cleanup(fx);
}

TEST_CASE("templates path honours $PLANAR_TEMPLATES_DIR over the default", "[cmd][templates][path]") {
  auto fx = make_fixture("pathenv");
  fx.vars.emplace("PLANAR_TEMPLATES_DIR", (fx.root / "elsewhere").string());
  auto const r = dispatch(fx, {"templates", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == (fx.root / "elsewhere").string() + "\n");
  cleanup(fx);
}

TEST_CASE("templates path honours [templates] dir from the config file", "[cmd][templates][path]") {
  auto const fx = make_fixture("pathcfg");
  write_file(fx.root / "config.toml", std::format("[templates]\ndir = \"{}\"\n", (fx.root / "fromcfg").string()));
  auto const r = dispatch(fx, {"templates", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == (fx.root / "fromcfg").string() + "\n");
  cleanup(fx);
}

TEST_CASE("templates path lets $PLANAR_TEMPLATES_DIR WIN over the config file", "[cmd][templates][path]") {
  // Precedence, asserted with BOTH sources present and DIFFERENT. Either
  // one alone would pass against an implementation that read only the
  // other.
  auto fx = make_fixture("pathprec");
  write_file(fx.root / "config.toml", std::format("[templates]\ndir = \"{}\"\n", (fx.root / "fromcfg").string()));
  fx.vars.emplace("PLANAR_TEMPLATES_DIR", (fx.root / "fromenv").string());
  CHECK(dispatch(fx, {"templates", "path"}).out == (fx.root / "fromenv").string() + "\n");
  cleanup(fx);
}

// --- templates list ---------------------------------------------------

TEST_CASE("templates list shows the embedded set when nothing is on disk", "[cmd][templates][list]") {
  auto const fx = make_fixture("list");
  auto const r  = dispatch(fx, {"templates", "list"});
  CHECK(r.code == 0);
  CHECK_FALSE(r.db_open);
  // Header + rule + one line per embedded template.
  CHECK(r.out.starts_with("SET                  SYSTEM               KIND                 SOURCE\n"));
  CHECK(r.out.find("default              jira                 epic                 embedded\n") != std::string::npos);
  CHECK(line_count(r.out) > 3);
  cleanup(fx);
}

TEST_CASE("templates list --json is NDJSON with no enclosing array", "[cmd][templates][list]") {
  auto const fx = make_fixture("listjson");
  auto const r  = dispatch(fx, {"templates", "list", "--json"});
  CHECK(r.code == 0);
  CHECK_FALSE(r.out.starts_with("["));
  CHECK(r.out.starts_with(R"({"set":"default",)"));
  CHECK(r.out.ends_with("}\n"));
  // Every line is its own complete object.
  CHECK(line_count(r.out) >= 3);
  cleanup(fx);
}

TEST_CASE("templates list --system EXCLUDES the non-matching rows", "[cmd][templates][list][filter]") {
  // Three answers that must all differ. An inert filter gives the first
  // answer three times; a filter that dropped everything gives the third
  // three times. Only a real filter produces all three.
  auto const fx         = make_fixture("listfilter");
  auto const unfiltered = dispatch(fx, {"templates", "list", "--json"});
  auto const jira       = dispatch(fx, {"templates", "list", "--system", "jira", "--json"});
  auto const nomatch    = dispatch(fx, {"templates", "list", "--system", "nosuch", "--json"});

  CHECK(line_count(unfiltered.out) > line_count(jira.out));
  CHECK(line_count(jira.out) > 0);
  // The survivor is NAMED, not counted.
  CHECK(jira.out.find(R"("system":"jira")") != std::string::npos);
  // And the excluded family is GONE.
  CHECK(jira.out.find(R"("system":"github-issues")") == std::string::npos);
  CHECK(unfiltered.out.find(R"("system":"github-issues")") != std::string::npos);
  CHECK(nomatch.out.empty());
  cleanup(fx);
}

TEST_CASE("templates list treats an EMPTY --system as NO filter", "[cmd][templates][list][filter]") {
  // `--system ""` lists everything, identically to omitting the flag.
  // Oracle-captured. This is the empty-value shape that produced a
  // withdrawn false-positive defect report in this milestone; it is
  // pinned rather than assumed.
  auto const fx = make_fixture("listempty");
  CHECK(dispatch(fx, {"templates", "list", "--json"}).out == dispatch(fx, {"templates", "list", "--system", "", "--json"}).out);
  CHECK(dispatch(fx, {"templates", "list", "--json"}).out == dispatch(fx, {"templates", "list", "--set", "", "--json"}).out);
  cleanup(fx);
}

TEST_CASE("templates list on NO matches is a SENTENCE in text and ZERO BYTES in json", "[cmd][templates][list][empty]") {
  // The two spellings disagree, deliberately. `[]` for the second would be
  // valid JSON and a parity break.
  auto const fx   = make_fixture("listnone");
  auto const text = dispatch(fx, {"templates", "list", "--system", "nosuch"});
  CHECK(text.code == 0);
  CHECK(text.out == "no templates found\n");

  auto const json = dispatch(fx, {"templates", "list", "--system", "nosuch", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.empty());
  cleanup(fx);
}

TEST_CASE("templates list reports a DISK row once init has written it", "[cmd][templates][list]") {
  // The `embedded` -> `disk` flip is the operator's signal that they now
  // own the files, so the dedup direction is observable at the leaf.
  auto const fx = make_fixture("listdisk");
  CHECK(dispatch(fx, {"templates", "list", "--json"}).out.find(R"("source":"embedded")") != std::string::npos);
  REQUIRE(dispatch(fx, {"templates", "init"}).code == 0);
  auto const after = dispatch(fx, {"templates", "list", "--json"});
  CHECK(after.out.find(R"("source":"disk")") != std::string::npos);
  CHECK(after.out.find(R"("source":"embedded")") == std::string::npos);
  cleanup(fx);
}

// --- templates show ---------------------------------------------------

TEST_CASE("templates show writes the template's RAW bytes", "[cmd][templates][show]") {
  // Not a DOM round trip: the operator's own formatting survives. The
  // shipped `issue.json` keeps `"labels": ["planar-managed", "type:task"]`
  // on ONE line, which a re-encode would break across four.
  auto const fx = make_fixture("show");
  auto const r  = dispatch(fx, {"templates", "show", "default", "github-issues", "issue"});
  CHECK(r.code == 0);
  CHECK_FALSE(r.db_open);
  CHECK(r.out.find(R"("labels": ["planar-managed", "type:task"])") != std::string::npos);
  CHECK(r.out.ends_with("\n"));
  cleanup(fx);
}

TEST_CASE("templates show prefers a DISK override over the embedded default", "[cmd][templates][show]") {
  auto const fx = make_fixture("showdisk");
  write_file(templates_root(fx) / "mine" / "jira" / "epic.json", R"({"mine":true})");
  auto const r = dispatch(fx, {"templates", "show", "mine", "jira", "epic"});
  CHECK(r.code == 0);
  CHECK(r.out == "{\"mine\":true}\n");
  cleanup(fx);
}

TEST_CASE("templates show falls THROUGH an unparseable override to the embedded default", "[cmd][templates][show]") {
  // A truncated operator override must not permanently shadow a working
  // baseline. This is the F4 remediation the loader carries, exercised
  // from the leaf.
  auto const fx = make_fixture("showbadjson");
  write_file(templates_root(fx) / "mine" / "jira" / "epic.json", "{ truncated");
  auto const r = dispatch(fx, {"templates", "show", "mine", "jira", "epic"});
  CHECK(r.code == 0);
  CHECK(r.out.find("customfield_10004") != std::string::npos);
  cleanup(fx);
}

TEST_CASE("templates show treats a DUPLICATE-KEY override as unparseable", "[cmd][templates][show][duplicate]") {
  // `std.json` rejects duplicate object keys (`duplicate_field_behavior`
  // defaults to `.@"error"`), so the candidate never wins its level.
  //
  // This case is why the tree has ONE JSON parser: the loader used to ask
  // Glaze, which ACCEPTS duplicates, and the leaf rendered the last value
  // at exit 0 where the oracle answered `not found` at exit 1. Captured by
  // a differential run over the whole family.
  auto const fx = make_fixture("showdup");
  write_file(templates_root(fx) / "dup" / "px" / "k.json", R"({"a":"first","a":"second"})");
  auto const r = dispatch(fx, {"templates", "show", "dup", "px", "k"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: template dup/px/k not found\n");
  cleanup(fx);
}

TEST_CASE("templates show REFUSES an unknown template at exit 1", "[cmd][templates][show]") {
  auto const fx = make_fixture("shownone");
  auto const r  = dispatch(fx, {"templates", "show", "default", "github-issues", "nosuch"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: template default/github-issues/nosuch not found\n");
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

// --- templates validate -----------------------------------------------

TEST_CASE("templates validate accepts every SHIPPED template", "[cmd][templates][validate]") {
  // The strongest single assertion in this file: the renderer's supported
  // directive surface must cover everything Planar actually ships, or
  // `ext propagate` is broken for that system.
  auto const fx = make_fixture("validateall");
  for (auto const& [system, kind] : std::vector<std::pair<std::string, std::string>>{{"github-issues", "issue"},
                                                                                     {"github-issues", "parent-issue"},
                                                                                     {"github-issues", "sub-task"},
                                                                                     {"github-issues", "test-scenario"},
                                                                                     {"github-issues", "tracking-issue"},
                                                                                     {"github-projects", "project"},
                                                                                     {"jira", "epic"},
                                                                                     {"jira", "story"},
                                                                                     {"jira", "sub-task"},
                                                                                     {"jira", "test-scenario"}}) {
    INFO("template: " << system << "/" << kind);
    auto const r = dispatch(fx, {"templates", "validate", "default", system, kind});
    CHECK(r.code == 0);
    CHECK(r.out == std::format("ok: default/{}/{}\n", system, kind));
    CHECK_FALSE(r.db_open);
  }
  cleanup(fx);
}

TEST_CASE("templates validate reports issues on STDOUT and the count on STDERR at exit 2", "[cmd][templates][validate]") {
  // The split is the oracle's and both halves are pinned: a caller reading
  // only stderr sees the count and not the detail. Exit 2 (invalid_input),
  // NOT exit 1.
  auto const fx   = make_fixture("validatebad");
  auto const path = templates_root(fx) / "probe" / "px" / "broken.json";
  write_file(path, R"({"a":"{{.Nope}}","b":"{{.Touches}}"})");

  auto const r = dispatch(fx, {"templates", "validate", "probe", "px", "broken"});
  CHECK(r.code == 2);
  // One line per field, plus the whole-template smoke pass — the oracle
  // reports a broken template TWICE and the count reflects it.
  CHECK(r.out == std::format("ISSUE: {} [a]: UnknownField\n"
                             "ISSUE: {} [b]: UnsupportedDirective\n"
                             "ISSUE: {} [(smoke-render)]: smoke render failed: UnknownField\n",
                             path.string(), path.string(), path.string()));
  CHECK(r.err == "error: 3 issue(s) in probe/px/broken\n");
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

TEST_CASE("templates validate --json emits its envelope on STDOUT and still exits 2", "[cmd][templates][validate]") {
  auto const fx = make_fixture("validatejson");
  write_file(templates_root(fx) / "probe" / "px" / "broken.json", R"({"a":"{{.Nope}}"})");
  auto const r = dispatch(fx, {"templates", "validate", "probe", "px", "broken", "--json"});
  CHECK(r.code == 2);
  CHECK(r.out.starts_with(R"({"ok":false,"set":"probe","system":"px","kind":"broken","issues":[)"));
  CHECK(r.out.ends_with("]}\n"));
  CHECK(r.err == "error: 2 issue(s) in probe/px/broken\n");
  cleanup(fx);
}

TEST_CASE("templates validate reports the REQUESTED set even when it resolved elsewhere", "[cmd][templates][validate]") {
  // `validate myset jira epic` with no `myset` on disk resolves to the
  // EMBEDDED default and still reports `myset`. Oracle-captured.
  auto const fx = make_fixture("validateset");
  auto const r  = dispatch(fx, {"templates", "validate", "myset", "jira", "epic", "--json"});
  CHECK(r.code == 0);
  CHECK(r.out == R"({"ok":true,"set":"myset","system":"jira","kind":"epic","issues":[]})"
                 "\n");
  cleanup(fx);
}

TEST_CASE("templates validate REFUSES an unknown template at exit 1, not exit 2", "[cmd][templates][validate]") {
  // A MISSING template and a BROKEN one are different failures with
  // different exit codes. Collapsing them would make "no such template"
  // indistinguishable from "template has issues" to a script.
  auto const fx = make_fixture("validatenone");
  auto const r  = dispatch(fx, {"templates", "validate", "default", "github-issues", "nosuch"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: template default/github-issues/nosuch not found\n");
  cleanup(fx);
}

// --- templates init ---------------------------------------------------

TEST_CASE("templates init extracts the embedded set under <root>/default/", "[cmd][templates][init]") {
  auto const fx = make_fixture("init");
  auto const r  = dispatch(fx, {"templates", "init"});
  CHECK(r.code == 0);
  CHECK_FALSE(r.db_open);
  CHECK(r.out.starts_with("templates init: wrote "));
  CHECK(r.out.find(" file(s)\n") != std::string::npos);
  // The PATH SHAPE is contract — `templates list` finds these by walking
  // exactly this hierarchy.
  CHECK(std::filesystem::exists(templates_root(fx) / "default" / "jira" / "epic.json"));
  CHECK(std::filesystem::exists(templates_root(fx) / "default" / "github-issues" / "issue.json"));
  cleanup(fx);
}

TEST_CASE("templates init is IDEMPOTENT and says so", "[cmd][templates][init][empty]") {
  auto const fx = make_fixture("initagain");
  REQUIRE(dispatch(fx, {"templates", "init"}).code == 0);
  auto const second = dispatch(fx, {"templates", "init"});
  CHECK(second.code == 0);
  CHECK(second.out == "templates init: nothing to do (all templates already present)\n");

  auto const json = dispatch(fx, {"templates", "init", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.empty());
  cleanup(fx);
}

TEST_CASE("templates init --force does NOT overwrite — reproduced defect", "[cmd][templates][init][oracle-defect]") {
  // The flag is declared and DISCARDED (`_ = args.force;` in the zig
  // handler), so a populated root reports nothing-to-do under `--force`
  // and the operator's edits survive. Oracle-captured on a third run.
  //
  // Reproduced under D2, not corrected. It is arguably wrong for a flag
  // named `--force`, but the fix belongs against the oracle; a one-sided
  // change here would diverge the two trees on a surface that WRITES
  // FILES.
  auto const fx = make_fixture("initforce");
  REQUIRE(dispatch(fx, {"templates", "init"}).code == 0);

  auto const target = templates_root(fx) / "default" / "jira" / "epic.json";
  write_file(target, "OPERATOR EDIT");

  auto const forced = dispatch(fx, {"templates", "init", "--force"});
  CHECK(forced.code == 0);
  CHECK(forced.out == "templates init: nothing to do (all templates already present)\n");

  std::ifstream     file(target, std::ios::binary);
  std::string const body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  CHECK(body == "OPERATOR EDIT");
  cleanup(fx);
}

TEST_CASE("templates init --json is NDJSON of the paths it wrote", "[cmd][templates][init]") {
  auto const fx = make_fixture("initjson");
  auto const r  = dispatch(fx, {"templates", "init", "--json"});
  CHECK(r.code == 0);
  CHECK(r.out.starts_with(R"({"path":")"));
  CHECK_FALSE(r.out.starts_with("["));
  CHECK(line_count(r.out) >= 3);
  cleanup(fx);
}

// --- templates render -------------------------------------------------
//
// These are the only cases that touch SQLite. The `--entity` REFUSALS are
// checked BEFORE the database is opened, which is asserted rather than
// assumed — a typo must refuse identically with or without a usable
// database, and `db_open == false` is the only way to see that.

TEST_CASE("templates render refuses a malformed --entity BEFORE opening the database", "[cmd][templates][render][refusal]") {
  auto const fx = make_fixture("renderbadref");

  auto const nocolon = dispatch(fx, {"templates", "render", "default", "jira", "epic", "--entity", "task1"});
  CHECK(nocolon.code == 2);
  CHECK(nocolon.err == "error: --entity must be kind:id (got 'task1')\n");
  CHECK_FALSE(nocolon.db_open);

  auto const badid = dispatch(fx, {"templates", "render", "default", "jira", "epic", "--entity", "task:abc"});
  CHECK(badid.code == 2);
  CHECK(badid.err == "error: --entity id must be an integer (got 'abc')\n");
  CHECK_FALSE(badid.db_open);

  auto const badkind = dispatch(fx, {"templates", "render", "default", "jira", "epic", "--entity", "nope:1"});
  CHECK(badkind.code == 2);
  CHECK(badkind.err == "error: unsupported entity kind 'nope' (use task, plan, scenario)\n");
  CHECK_FALSE(badkind.db_open);

  cleanup(fx);
}

TEST_CASE("templates render refuses an unknown TEMPLATE before touching --entity at all", "[cmd][templates][render][refusal]") {
  // Ordering is observable: a bad template AND a bad entity together
  // reports the TEMPLATE, and never opens the database.
  auto const fx = make_fixture("rendernotmpl");
  auto const r  = dispatch(fx, {"templates", "render", "default", "jira", "nosuch", "--entity", "garbage"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: template default/jira/nosuch not found\n");
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

TEST_CASE("templates render OPENS the database for a well-formed entity ref", "[cmd][templates][render]") {
  // The positive half of the `db_open` contract. Without it, the five
  // `CHECK_FALSE(db_open)` assertions above would also pass for a build
  // where `ensure_db` never worked at all.
  auto const fx = make_fixture("renderopens");
  auto const r  = dispatch(fx, {"templates", "render", "default", "jira", "epic", "--entity", "task:1"});
  CHECK(r.db_open);
  // No such task in a fresh database, so it refuses — but it got far
  // enough to look.
  CHECK(r.code == 1);
  CHECK(r.err == "error: building task context: NotFound\n");
  cleanup(fx);
}
