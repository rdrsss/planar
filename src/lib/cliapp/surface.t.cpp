// @file surface.t.cpp
// @brief Tests for `planar.cliapp.surface` — specifically the `--no-X`
// negation every boolean flag now carries (plan 996, task 6138).
//
// ## Oracle provenance
//
// Every expectation below is a transcription of what
// `zig/zig-out/bin/planar` did under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH, not what its `--help` claims
// and not what `zig/vendor/etcli-zig/src/cli/parser.zig` reads like:
//
//   $Z task list --no-json                exit 1  (reached the engine)
//   $Z task add T --due bad --no-editor   exit 1  error: task add: InvalidDueAt
//                                                 (--editor DEFAULTS TRUE, and the
//                                                  negation is accepted anyway)
//   $Z task list --json --no-json         exit 2  error: flag specified more than once: --json
//   $Z task list --json --json            exit 2  error: flag specified more than once: --json
//   $Z task list --no-json=true           exit 2  error: unknown flag (got --no-json=true)
//   $Z task list --no-no-json             exit 2  error: unknown flag (got --no-no-json)
//   $Z task list --no-scope x             exit 2  error: unknown flag (got --no-scope)
//   $Z workbench list --json=bogus        exit 2  error: invalid value: --json
//   $Z workbench list --json=false        exit 0  human output, NOT JSON
//   $A claim --entity task:1 --no-no-transition
//                                         exit 1  (accepted — `--no-transition` is
//                                                  itself a bool flag, so the parser
//                                                  synthesizes ITS negation too)
//
// The last row is the one that decides the rule's shape: there is no
// "flags already named `--no-*` are exempt" carve-out to write, because
// the oracle has none.
//
// ## Break-probes run against this file
//
//   - Dropped the `,!--no-` half of `add_bool_flag`'s name string ->
//     `a boolean flag accepts its negated form` FAILS (CLI11 throws
//     ExtrasError), and so do the default-true and double-negation cases.
//     Restored, touched, rebuilt -> green.
//   - Changed `add_bool_flag`'s prefix from `!--no-` to a plain `--no-`
//     (a non-negating second name) -> `a negated boolean flag records
//     false` FAILS: the result string is "true" where "false" is
//     required, which is exactly the silent-wrong-value shape a
//     presence-only assertion would have missed. Restored -> green.
//   - Removed the `get_fnames()` subtraction from
//     `planar.cliapp.schema::aliases_of` -> `the negation is invisible to
//     the catalog` FAILS with `"aliases":["--no-json"]`. Restored ->
//     green.
//   - Deleted the bool-literal `check` -> `an unrecognised inline boolean
//     value is refused` FAILS (parse succeeds). Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;
import planar.cliapp.surface;

namespace {

using planar::cliapp::add_bool_flag;

/// @brief Parse `args` against a freshly built one-leaf tree.
/// @param declare Called with the leaf so a case can shape it.
/// @param args The operator tokens, argv[0] excluded.
/// @return The harvested values, or unset when CLI11 refused the parse.
auto try_parse(auto&& declare, std::vector<std::string> args) -> std::optional<planar::cliapp::parsed_args> {
  CLI::App app{"", "tool"};
  app.require_subcommand(0);
  CLI::App* run = app.add_subcommand("run", "Run it");
  declare(*run);

  args.insert(args.begin(), "run");
  // CLI11's vector overload consumes the tokens in REVERSE order.
  std::vector<std::string> reversed(args.rbegin(), args.rend());
  try {
    app.parse(std::move(reversed));
  } catch (const CLI::ParseError&) {
    return std::nullopt;
  }
  return planar::cliapp::harvest(app);
}

/// @brief The plain-`--json` leaf every case below starts from.
/// @param leaf The node to declare it on.
auto declare_json(CLI::App& leaf) -> void {
  add_bool_flag(leaf, "--json");
}

} // namespace

TEST_CASE("a boolean flag still accepts its plain form", "[cliapp][surface][negation]") {
  auto const got = try_parse(declare_json, {"--json"});
  REQUIRE(got.has_value());
  CHECK(planar::cliapp::flag_bool(*got, "--json"));
}

TEST_CASE("a boolean flag accepts its negated form", "[cliapp][surface][negation]") {
  // Before task 6138 this was `error: ExtrasError`, exit 2 — the whole
  // point of the task. The oracle has always accepted it.
  auto const got = try_parse(declare_json, {"--no-json"});
  REQUIRE(got.has_value());
}

TEST_CASE("a negated boolean flag records false, not merely presence", "[cliapp][surface][negation]") {
  auto const got = try_parse(declare_json, {"--no-json"});
  REQUIRE(got.has_value());
  // The assertion that discriminates. `flag_bool` used to answer "is the
  // key present", which is TRUE here — so a presence-only check passes
  // against a parser that silently ignores the negation.
  CHECK_FALSE(planar::cliapp::flag_bool(*got, "--json"));
}

TEST_CASE("an absent boolean flag falls back rather than reading false", "[cliapp][surface][negation]") {
  auto const got = try_parse(declare_json, {});
  REQUIRE(got.has_value());
  CHECK_FALSE(planar::cliapp::flag_bool(*got, "--json"));
  CHECK(planar::cliapp::flag_bool(*got, "--json", true));
}

TEST_CASE("a boolean flag DEFAULTING TRUE accepts its negation and goes false", "[cliapp][surface][negation]") {
  // `planar task add --editor` and `planar artifact add --editor` are the
  // two real instances. This is the case the old presence-only `flag_bool`
  // could not express at all: `harvest` seeds the key from the declared
  // default, so absent and `--no-editor` looked identical.
  auto const declare = [](CLI::App& leaf) { add_bool_flag(leaf, "--editor")->default_str("true"); };

  auto const absent = try_parse(declare, {});
  REQUIRE(absent.has_value());
  CHECK(planar::cliapp::flag_bool(*absent, "--editor"));

  auto const negated = try_parse(declare, {"--no-editor"});
  REQUIRE(negated.has_value());
  CHECK_FALSE(planar::cliapp::flag_bool(*negated, "--editor"));
}

TEST_CASE("a flag already named no-something gets its own negation too", "[cliapp][surface][negation]") {
  // `planar-agent claim --no-transition` is a genuinely DECLARED bool
  // flag, and the oracle synthesizes `--no-no-transition` for it like any
  // other. Verified by running it, not inferred from the parser source.
  auto const declare = [](CLI::App& leaf) { add_bool_flag(leaf, "--no-transition"); };

  auto const plain = try_parse(declare, {"--no-transition"});
  REQUIRE(plain.has_value());
  CHECK(planar::cliapp::flag_bool(*plain, "--no-transition"));

  auto const doubled = try_parse(declare, {"--no-no-transition"});
  REQUIRE(doubled.has_value());
  CHECK_FALSE(planar::cliapp::flag_bool(*doubled, "--no-transition"));
}

TEST_CASE("a non-boolean flag gets no negation", "[cliapp][surface][negation]") {
  // etcli's negation arm is guarded on `f.kind == .bool`; `--no-scope` is
  // an unknown flag on the oracle. Widening the rule to every flag would
  // be the easy over-correction.
  auto const declare = [](CLI::App& leaf) { leaf.add_option("--scope"); };
  CHECK_FALSE(try_parse(declare, {"--no-scope", "x"}).has_value());
}

TEST_CASE("a doubly negated form is refused, matching the oracle", "[cliapp][surface][negation]") {
  CHECK_FALSE(try_parse(declare_json, {"--no-no-json"}).has_value());
}

TEST_CASE("an unrecognised inline boolean value is refused", "[cliapp][surface][negation]") {
  // `--json=bogus` was exit 0 here and exit 2 on the oracle before task
  // 6138 attached the bool-literal validator. Not a negation bug, but the
  // same declaration site owns it.
  CHECK_FALSE(try_parse(declare_json, {"--json=bogus"}).has_value());
}

TEST_CASE("an inline false is honoured rather than read as presence", "[cliapp][surface][negation]") {
  auto const got = try_parse(declare_json, {"--json=false"});
  REQUIRE(got.has_value());
  CHECK_FALSE(planar::cliapp::flag_bool(*got, "--json"));
}

TEST_CASE("the negation is invisible to the schema catalog", "[cliapp][surface][negation]") {
  // The constraint that makes the whole approach viable: `planar-agent`
  // and `planar-watch` emit catalogs BYTE-IDENTICAL to the oracle's, and
  // the oracle declares no negations at all. CLI11 files a `!`-prefixed
  // name into `lnames_` as well as `fnames_`, so this fails loudly the
  // moment `aliases_of` stops subtracting.
  CLI::App app{"", "tool"};
  add_bool_flag(app, "--json");
  auto const catalog = planar::cliapp::schema_json(app);
  CHECK(catalog.contains(R"("long":"--json")"));
  CHECK(catalog.contains(R"("aliases":[])"));
  CHECK_FALSE(catalog.contains("--no-json"));
  // Still a bool, not reclassified by the validator the flag now carries.
  CHECK(catalog.contains(R"("kind":"bool")"));
}

TEST_CASE("apply_surface declares negations for generated bool flags too", "[cliapp][surface][negation]") {
  // The two halves of every tree — hand-written `tree.cpp` and generated
  // `surface.cpp` — must agree, and only the generated half goes through
  // `declare_flag`. A fix applied to one half only is the likeliest way to
  // half-close this task.
  static constexpr std::string_view          k_path[]  = {"gen"};
  static constexpr planar::cliapp::flag_spec k_flags[] = {
      {.name = "--verbose", .kind = "bool"},
      {.name = "--label", .kind = "string"},
  };
  static constexpr planar::cliapp::node_spec k_nodes[] = {
      {.path = k_path, .description = "Generated leaf", .flags = k_flags},
  };

  CLI::App app{"", "tool"};
  app.require_subcommand(0);
  auto const created = planar::cliapp::apply_surface(app, k_nodes);
  REQUIRE(created.size() == 1);

  std::vector<std::string> reversed{"--no-verbose", "gen"};
  REQUIRE_NOTHROW(app.parse(std::move(reversed)));
  auto const got = planar::cliapp::harvest(app);
  CHECK_FALSE(planar::cliapp::flag_bool(got, "--verbose"));
}

// ---------------------------------------------------------------------------
// Position-independent flags (plan 996, task 6131 item 1).
// ---------------------------------------------------------------------------
//
// ORACLE PROVENANCE, captured under a scratch DB. The task recorded this
// as "a global flag BEFORE the subcommand"; running the oracle shows it is
// not about the root and not about being first:
//
//   $Z --json health              exit 0, the health JSON
//   $Z --json version             exit 2, error: unknown flag (got --json)
//   $Z --bogus health             exit 2, error: unknown flag (got --bogus)
//   $Z workbench --json list      exit 0  (MID-PATH)
//   $Z --scope oracle task list   exit 1, SlugNotFound
//                                 (so `oracle` was --scope's VALUE, not a verb)
//
// The middle two are what make this safe to MATCH rather than record as a
// divergence: the oracle is not permissive about undeclared flags at all,
// only indifferent to where a DECLARED one appears.
//
// ## Break-probes run against this block
//
//   - Made `hoist_subcommands` return `argv` unchanged -> `a flag before
//     the subcommand still resolves the subcommand` and the mid-path case
//     both FAIL. Restored, touched, rebuilt -> green.
//   - Widened the subcommand lookup from the CURRENT node to the whole
//     tree -> `a leaf positional is not mistaken for a verb` FAILS
//     (`other` is hoisted out of `inner`'s positional slot and the
//     invocation silently retargets). Restored -> green. An earlier
//     `!children(*node).empty()` guard was probed here too and proved
//     DEAD — a leaf's subcommand list is already empty — so it was
//     removed rather than kept as unearned reassurance.
//   - Dropped the `--` passthrough latch -> `a token after the terminator
//     is never a verb` FAILS. Restored -> green.

namespace {

/// @brief A two-level tree: `outer` with children `inner` and `other`,
/// where `inner` is a leaf carrying a positional NAMED LIKE ITS SIBLING.
/// @param app The root to build into.
auto build_hoist_tree(CLI::App& app) -> void {
  app.require_subcommand(0);
  CLI::App* outer = app.add_subcommand("outer", "Outer");
  outer->require_subcommand(0);
  CLI::App* inner = outer->add_subcommand("inner", "Inner");
  add_bool_flag(*inner, "--json");
  inner->add_option("name");
  outer->add_subcommand("other", "Other");
}

/// @brief Hoist `args` (argv[0] excluded) against `build_hoist_tree`.
/// @param args The operator tokens.
/// @return The reordered tokens, argv[0] excluded.
auto hoist(std::vector<std::string> args) -> std::vector<std::string> {
  CLI::App app{"", "tool"};
  build_hoist_tree(app);
  args.insert(args.begin(), "tool");
  auto out = planar::cliapp::hoist_subcommands(app, args);
  out.erase(out.begin());
  return out;
}

} // namespace

TEST_CASE("a flag before the subcommand still resolves the subcommand", "[cliapp][surface][hoist]") {
  CHECK(hoist({"--json", "outer", "inner"}) == std::vector<std::string>{"outer", "inner", "--json"});
}

TEST_CASE("a flag in the MIDDLE of the path is hoisted the same way", "[cliapp][surface][hoist]") {
  CHECK(hoist({"outer", "--json", "inner"}) == std::vector<std::string>{"outer", "inner", "--json"});
}

TEST_CASE("a flag and its value keep their relative order", "[cliapp][surface][hoist]") {
  // The property that lets this work WITHOUT knowing which flags take a
  // value: `--scope` and `oracle` are adjacent before and after, so the
  // leaf's own parser pairs them exactly as it always did.
  CHECK(hoist({"--scope", "oracle", "outer", "inner"}) == std::vector<std::string>{"outer", "inner", "--scope", "oracle"});
}

TEST_CASE("a leaf positional is not mistaken for a verb", "[cliapp][surface][hoist]") {
  // `inner` is a LEAF, so its `name` positional must survive even when it
  // spells `other`, a sibling verb. Hoisting it would silently retarget
  // the invocation — the worst failure this function could have.
  //
  // The interposed `--json` is what makes the failure OBSERVABLE, and it
  // is here because the obvious form of this case is vacuous: without it,
  // `{outer, inner, other}` is already in hoisted order, so an
  // implementation that wrongly promotes `other` into the PATH emits the
  // very same four tokens and the assertion passes. A break-probe caught
  // exactly that. With the flag in between, promoting `other` moves it
  // ahead of `--json` and the sequences differ.
  CHECK(hoist({"outer", "inner", "--json", "other"}) == std::vector<std::string>{"outer", "inner", "--json", "other"});
}

TEST_CASE("a token after the terminator is never a verb", "[cliapp][surface][hoist]") {
  CHECK(hoist({"outer", "--", "inner"}) == std::vector<std::string>{"outer", "--", "inner"});
}

TEST_CASE("an already ordered argv is left exactly as it was", "[cliapp][surface][hoist]") {
  CHECK(hoist({"outer", "inner", "--json"}) == std::vector<std::string>{"outer", "inner", "--json"});
  CHECK(hoist({}) == std::vector<std::string>{});
}
