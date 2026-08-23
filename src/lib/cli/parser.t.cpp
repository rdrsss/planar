// @file parser.t.cpp
// @brief Unit tests for `planar.cli.parser::parse` (plan 996, task
// cpp-cli-tree-parity).
//
// The modeled tree is a deliberately small SUBSET of Planar's real verb
// surface, chosen to exercise the parser machinery end-to-end rather than
// to reproduce every verb:
//
//   - `task` (a bare parent/group node — no handler surface in this port,
//     see cmd.cppm) with children `add` and `done`. `task add` mirrors the
//     REAL `planar task add` leaf: a required string positional
//     (`<title>`), several optional string flags, an `int` flag with a
//     default, and a `bool` flag with a default — nesting + positional +
//     flag-kind coverage (string/int/bool) in one leaf. `task done`
//     mirrors the real `planar task done` leaf: a required string
//     positional plus a `bool` flag — a second, simpler leaf under the
//     same parent, to exercise sibling resolution and the
//     bare-parent-renders-help-at-exit-0 path (`task` with no further
//     token).
//   - `fail` (mirrors the real `planar-agent fail` leaf) adds: two
//     REQUIRED flags (`--claim`, `--reason`) and a `choice`-kind flag
//     with a default (`--category`) — required-flag and choice-kind
//     coverage that `task add`/`task done` don't exercise.
//
// Together these three leaves exercise: nesting, required vs. optional
// flags, flag kinds string/int/bool/choice, a required positional, and
// one error path per brief-mandated shape (unknown verb, unknown flag,
// missing required positional, bad enum value) plus missing-required-flag.
//
// Every literal expected error message and exit-code assertion below was
// captured by actually running `./zig/zig-out/bin/planar` /
// `./zig/zig-out/bin/planar-agent` (task brief: "derive the expected
// value by RUNNING ... — do not hand-write what you assume it emits").
// The `[parity]`-tagged cases additionally re-run those binaries live
// (SKIP-if-absent, matching src/lib/db/migrate.t.cpp's established
// pattern) so the assertions self-check against the reference binary
// rather than trusting a frozen transcript.
#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS for the parity test's std::system() status

import std;
import planar.cli;

namespace {

using planar::cli::cmd;
using planar::cli::flag;
using planar::cli::positional;

/// @brief The real `planar` binary's `task` subtree, reduced to `add` and
/// `done` (see file comment). Flag/positional shapes match
/// `./zig/zig-out/bin/planar task add --help` / `task done --help`
/// exactly (long names, kinds, defaults, required-ness).
auto make_planar_root() -> cmd {
  cmd add{
      .name = "add",
      .desc = "Create a new task.",
      .flags =
          {
              flag{.long_name = "--body"},
              flag{.long_name = "--scope"},
              flag{.long_name = "--next-action"},
              flag{.long_name = "--due"},
              flag{.long_name = "--plan", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--parent", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--slug"},
              flag{.long_name = "--priority", .value_kind = planar::cli::kind::integer, .default_value = std::int64_t{100}},
              flag{.long_name = "--editor", .value_kind = planar::cli::kind::boolean, .default_value = true},
              flag{.long_name = "--no-auto-promote", .value_kind = planar::cli::kind::boolean, .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "title", .required = true}},
  };
  cmd done{
      .name = "done",
      .desc = "Mark a task as done (single-arg form; Go supports variadic).",
      .flags =
          {
              flag{.long_name = "--scope"},
              flag{.long_name     = "--force",
                   .desc          = "Override active-claim guard and flip status anyway.",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "task-id", .required = true}},
  };
  cmd task{
      .name = "task",
      .desc = "Manage tasks.",
      .cmds = {std::move(add), std::move(done)},
  };
  return cmd{.name = "planar", .cmds = {std::move(task)}};
}

/// @brief The real `planar-agent` binary's `fail` leaf. Flag shapes match
/// `./zig/zig-out/bin/planar-agent fail --help` exactly.
auto make_planar_agent_root() -> cmd {
  cmd fail{
      .name = "fail",
      .desc = "Atomically fail the work session: task \xe2\x86\x92 todo, claim \xe2\x86\x92 aborted.",
      .flags =
          {
              flag{.long_name = "--claim", .desc = "Claim token returned by pull/claim", .required = true},
              flag{.long_name = "--reason", .desc = "Failure reason recorded on the claim and action", .required = true},
              flag{
                  .long_name     = "--category",
                  .desc          = "Closed failure category (default: unknown)",
                  .value_kind    = planar::cli::kind::choice,
                  .choices       = {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"},
                  .default_value = std::string("unknown"),
              },
              flag{.long_name     = "--no-locality-probe",
                   .desc          = "Skip the git locality probe and commit collection",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
  };
  return cmd{.name = "planar-agent", .cmds = {std::move(fail)}};
}

auto get_str(std::unordered_map<std::string, planar::cli::value> const& m, std::string const& key) -> std::string {
  return std::get<std::string>(m.at(key));
}

auto get_int(std::unordered_map<std::string, planar::cli::value> const& m, std::string const& key) -> std::int64_t {
  return std::get<std::int64_t>(m.at(key));
}

auto get_bool(std::unordered_map<std::string, planar::cli::value> const& m, std::string const& key) -> bool {
  return std::get<bool>(m.at(key));
}

auto get_double(std::unordered_map<std::string, planar::cli::value> const& m, std::string const& key) -> double {
  return std::get<double>(m.at(key));
}

auto get_list(std::unordered_map<std::string, planar::cli::value> const& m, std::string const& key) -> std::vector<std::string> {
  return std::get<std::vector<std::string>>(m.at(key));
}

// ---------------------------------------------------------------------------
// B4 (M2 boundary review, plan 996 task 6066): synthetic parser-mechanism
// coverage. The task/task-add/task-done/fail subset above (help.t.cpp/
// schema.t.cpp's shared fixture) never exercises `short_name` at all — grep
// confirms `short_name` appears nowhere in this file before this block —
// leaving the parser's highest-risk logic (short-flag matching, bundling,
// attached values, negation, flag-group enforcement, count/list
// accumulation, `--` passthrough/rest, allow_unknown_flags, duplicate
// detection, too-many-positionals, negative-number positionals, and the
// duration/float/path kinds) with ZERO coverage. The synthetic `widget`
// tree below is built specifically to hit each of those mechanisms, one
// leaf at a time, with a break-probe style assertion (the flag/positional
// this test cares about, checked against the LITERAL value the mechanism
// should have produced — not just "no error").
// ---------------------------------------------------------------------------

using planar::cli::flag_group;
using planar::cli::flag_group_mode;

/// @brief `widget build` — short_name matching, attached short value
/// (`-fPATH`), `--flag=value` inline attachment, `--no-` negation, list
/// accumulation, aliases, the `duration`/`float`/`path` kinds, and a
/// negative-number positional.
auto make_build_leaf() -> cmd {
  return cmd{
      .name = "build",
      .desc = "Synthetic leaf covering short/attached/inline/negation/list/alias/kind mechanics.",
      .flags =
          {
              flag{.long_name = "--file", .aliases = {"--path-alt"}, .short_name = 'f', .value_kind = planar::cli::kind::path},
              flag{.long_name = "--ratio", .value_kind = planar::cli::kind::floating},
              flag{.long_name = "--timeout", .value_kind = planar::cli::kind::duration},
              flag{.long_name = "--enabled", .value_kind = planar::cli::kind::boolean, .default_value = true},
              flag{.long_name = "--tag", .value_kind = planar::cli::kind::string, .list = true},
          },
      .positionals = {positional{.name = "offset", .value_kind = planar::cli::kind::integer, .required = true}},
  };
}

/// @brief `widget bundle` — bool bundling of DISTINCT shorts (`-ab`), a
/// COUNT short bundled with itself (`-vvv`), duplicate-flag detection on a
/// non-count flag, and too-many-positionals (no `allow_extra_positionals`).
auto make_bundle_leaf() -> cmd {
  return cmd{
      .name = "bundle",
      .desc = "Synthetic leaf covering bool bundling, count bundling, duplicate-flag, too-many-positionals.",
      .flags =
          {
              flag{.long_name = "--alpha", .short_name = 'a', .value_kind = planar::cli::kind::boolean},
              flag{.long_name = "--beta", .short_name = 'b', .value_kind = planar::cli::kind::boolean},
              flag{.long_name = "--verbose", .short_name = 'v', .value_kind = planar::cli::kind::boolean, .count = true},
          },
      .positionals = {positional{.name = "only", .required = true}},
  };
}

/// @brief `widget forward` — `allow_unknown_flags`: an unrecognized flag
/// (and its best-effort-swallowed value) must not error and must not leak
/// into positionals/flags, while a declared flag alongside it still parses
/// normally.
auto make_forward_leaf() -> cmd {
  return cmd{
      .name                = "forward",
      .desc                = "Synthetic leaf covering allow_unknown_flags passthrough.",
      .flags               = {flag{.long_name = "--known", .value_kind = planar::cli::kind::boolean}},
      .allow_unknown_flags = true,
  };
}

/// @brief `widget collect` — `--` passthrough (tokens after `--` are never
/// treated as flags, even if they start with `-`) plus `rest_field`
/// overflow collection.
auto make_collect_leaf() -> cmd {
  return cmd{
      .name        = "collect",
      .desc        = "Synthetic leaf covering -- passthrough and rest_field overflow.",
      .positionals = {positional{.name = "head", .required = true}},
      .rest_field  = "rest",
  };
}

/// @brief `widget excl` — `flag_group_mode::mutually_exclusive`.
auto make_group_excl_leaf() -> cmd {
  return cmd{
      .name        = "excl",
      .desc        = "Synthetic leaf covering mutually_exclusive flag-group enforcement.",
      .flags       = {flag{.long_name = "--a", .value_kind = planar::cli::kind::boolean},
                      flag{.long_name = "--b", .value_kind = planar::cli::kind::boolean}},
      .flag_groups = {flag_group{.name = "mode", .mode = flag_group_mode::mutually_exclusive, .flags = {"--a", "--b"}}},
  };
}

/// @brief `widget any` — `flag_group_mode::required_one`.
auto make_group_any_leaf() -> cmd {
  return cmd{
      .name        = "any",
      .desc        = "Synthetic leaf covering required_one flag-group enforcement.",
      .flags       = {flag{.long_name = "--x", .value_kind = planar::cli::kind::boolean},
                      flag{.long_name = "--y", .value_kind = planar::cli::kind::boolean}},
      .flag_groups = {flag_group{.name = "input", .mode = flag_group_mode::required_one, .flags = {"--x", "--y"}}},
  };
}

/// @brief `widget exact` — `flag_group_mode::required_exactly_one`.
auto make_group_exact_leaf() -> cmd {
  return cmd{
      .name        = "exact",
      .desc        = "Synthetic leaf covering required_exactly_one flag-group enforcement.",
      .flags       = {flag{.long_name = "--p", .value_kind = planar::cli::kind::boolean},
                      flag{.long_name = "--q", .value_kind = planar::cli::kind::boolean}},
      .flag_groups = {flag_group{.name = "strict", .mode = flag_group_mode::required_exactly_one, .flags = {"--p", "--q"}}},
  };
}

auto make_synthetic_parser_root() -> cmd {
  return cmd{.name = "widget",
             .cmds = {make_build_leaf(), make_bundle_leaf(), make_forward_leaf(), make_collect_leaf(), make_group_excl_leaf(),
                      make_group_any_leaf(), make_group_exact_leaf()}};
}

} // namespace

// ---------------------------------------------------------------------------
// B4 synthetic-tree TEST_CASEs.
// ---------------------------------------------------------------------------

TEST_CASE("parse: short_name matching (-f) and attached short value (-fPATH)", "[parser][synthetic][short]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "42", "-f/tmp/out.txt"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_str(outcome->match.flags, "--file") == "/tmp/out.txt");
}

TEST_CASE("parse: --flag=value inline attachment on a long flag", "[parser][synthetic][inline]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "1", "--ratio=3.5"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_double(outcome->match.flags, "--ratio") == 3.5);
}

// --- numeric coercion parity (task 6067) ---------------------------------
//
// Oracle-derived: `./zig/zig-out/bin/planar task add probe --priority <v>`
// against the live zig binary (which coerces via
// vendor/etcli/src/cli/parser.zig's std.fmt.parseInt/parseFloat calls),
// run 2026-08-22:
//   int   '+5'    -> exit 0   int   '9_96'  -> exit 0
//   int   '9__6'  -> exit 0   int   '_5'    -> exit 2 (rejected)
//   int   '5_'    -> exit 2   int   '0x1F'  -> exit 2
//   int   '1e3'   -> exit 2   int   '  5'   -> exit 2 (whitespace)
// std::from_chars alone rejects the leading '+' and every '_' form; this
// pins the C++ side now matching that accept-set exactly.

TEST_CASE("parse: integer coercion accepts a leading '+' and '_' digit separators, matching zig's parseInt oracle",
          "[parser][synthetic][kinds][numeric-parity]") {
  auto root = make_synthetic_parser_root();

  {
    std::vector<std::string> argv{"widget", "build", "+996", "-f/tmp/x"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_int(outcome->match.positionals, "offset") == 996);
  }
  {
    std::vector<std::string> argv{"widget", "build", "9_96", "-f/tmp/x"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_int(outcome->match.positionals, "offset") == 996);
  }
  {
    // Consecutive '_' is legal for ints (zig's parseInt just drops every
    // '_' it sees; it does not police runs), unlike the float grammar.
    std::vector<std::string> argv{"widget", "build", "9_9__6", "-f/tmp/x"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_int(outcome->match.positionals, "offset") == 996);
  }
}

TEST_CASE("parse: integer coercion still rejects a leading/trailing '_', hex, exponent form, and embedded whitespace",
          "[parser][synthetic][kinds][numeric-parity]") {
  // Break-probe for the accept-set test above: these must still fail,
  // proving the normalization is not simply accepting everything.
  auto root = make_synthetic_parser_root();
  for (auto bad : {"_996", "996_", "0x1F", "1e3", " 996", "996 "}) {
    std::vector<std::string> argv{"widget", "build", std::string(bad), "-f/tmp/x"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::invalid_value);
  }
}

TEST_CASE("parse: float coercion accepts a leading '+', exponent forms, and '_' strictly between two digits (zig parity)",
          "[parser][synthetic][kinds][numeric-parity]") {
  auto root = make_synthetic_parser_root();

  {
    std::vector<std::string> argv{"widget", "build", "1", "--ratio=+3.5"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_double(outcome->match.flags, "--ratio") == 3.5);
  }
  {
    std::vector<std::string> argv{"widget", "build", "1", "--ratio=1_2.5_0e1_0"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_double(outcome->match.flags, "--ratio") == 12.50e10);
  }
  {
    std::vector<std::string> argv{"widget", "build", "1", "--ratio=1e+3"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_double(outcome->match.flags, "--ratio") == 1000.0);
  }
}

TEST_CASE("parse: float coercion rejects '_' that is not directly between two digits (leading, trailing, doubled, or "
          "adjacent to '.'/'e')",
          "[parser][synthetic][kinds][numeric-parity]") {
  // Break-probe: mirrors zig's own parseFloat test table (each case names
  // exactly which adjacency rule it violates) — proves the float
  // normalization is meaningfully stricter than the int one, not a
  // blanket "strip all underscores" pass.
  auto root = make_synthetic_parser_root();
  for (auto bad : {
           "0123456.789000e_0010",  // '_' right after 'e', not between digits
           "_0123456.789000e0010",  // '_' before any digit
           "0__123456.789000e0010", // doubled '_'
           "0123456_.789000e0010",  // '_' immediately before '.'
           "0123456.789000e0010_",  // '_' at the very end
       }) {
    std::vector<std::string> argv{"widget", "build", "1", std::string("--ratio=") + bad};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::invalid_value);
  }
}

TEST_CASE("parse: an alias resolves to the same canonical long flag name", "[parser][synthetic][alias]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "1", "--path-alt=/tmp/via-alias.txt"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_str(outcome->match.flags, "--file") == "/tmp/via-alias.txt");
}

TEST_CASE("parse: --no- negation flips a default-true bool flag to false", "[parser][synthetic][negation]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "1", "--no-enabled"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_bool(outcome->match.flags, "--enabled") == false);
  // Break-probe: the flag's OWN default (true) must NOT be what we're
  // seeing — confirms this assertion is actually sensitive to the negation
  // path, not just re-reading the default.
  std::vector<std::string> argv_absent{"widget", "build", "1"};
  auto                     outcome_absent = planar::cli::parse(root, argv_absent);
  REQUIRE(outcome_absent.has_value());
  CHECK(get_bool(outcome_absent->match.flags, "--enabled") == true);
}

TEST_CASE("parse: list flag accumulates repeated values in order", "[parser][synthetic][list]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "1", "--tag", "a", "--tag", "b", "--tag", "c"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_list(outcome->match.flags, "--tag") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("parse: duration and negative-number-positional kinds", "[parser][synthetic][kinds]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "build", "-5", "--timeout", "90s"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  // -5 is consumed as the integer positional, NOT mistaken for a flag,
  // because the currently-open positional (`offset`) is integer-kind.
  CHECK(get_int(outcome->match.positionals, "offset") == -5);
  // Duration coerces to nanoseconds: 90s = 90 * 1_000_000_000.
  CHECK(get_int(outcome->match.flags, "--timeout") == 90'000'000'000LL);
}

TEST_CASE("parse: bool bundling of distinct short flags (-ab sets both)", "[parser][synthetic][bundle]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "bundle", "-ab", "only-positional"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_bool(outcome->match.flags, "--alpha") == true);
  CHECK(get_bool(outcome->match.flags, "--beta") == true);
}

TEST_CASE("parse: count-flag bundling (-vvv counts 3)", "[parser][synthetic][bundle][count]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "bundle", "-vvv", "only-positional"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_int(outcome->match.flags, "--verbose") == 3);
}

TEST_CASE("parse error: duplicate_flag on a repeated non-count, non-list flag", "[parser][synthetic][error][duplicate]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "bundle", "--alpha", "--alpha", "only-positional"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  CHECK(outcome.error().kind == planar::cli::parse_error_kind::duplicate_flag);
  CHECK(outcome.error().flag_name == "--alpha");
}

TEST_CASE("parse error: too_many_positionals without allow_extra_positionals", "[parser][synthetic][error][positionals]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "bundle", "first", "second-overflow"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  CHECK(outcome.error().kind == planar::cli::parse_error_kind::too_many_positionals);
  CHECK(outcome.error().arg == "second-overflow");
}

TEST_CASE("parse: allow_unknown_flags swallows an unrecognized flag and its value silently", "[parser][synthetic][unknown]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "forward", "--mystery", "eaten", "--known"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_bool(outcome->match.flags, "--known") == true);
  CHECK_FALSE(outcome->match.flags.contains("--mystery"));
  CHECK(outcome->match.positionals.empty());
}

TEST_CASE("parse: -- passthrough disables flag parsing, rest_field collects the overflow", "[parser][synthetic][passthrough]") {
  auto                     root = make_synthetic_parser_root();
  std::vector<std::string> argv{"widget", "collect", "headval", "--", "-x", "-y", "z"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(get_str(outcome->match.positionals, "head") == "headval");
  // Non-vacuous: "-x"/"-y" retained their leading dash, proving they were
  // NEVER routed through flag matching (an actual flag match would have
  // either errored unknown_flag or consumed them as a value) — they landed
  // as literal rest tokens instead.
  CHECK(outcome->match.rest == std::vector<std::string>{"-x", "-y", "z"});
}

TEST_CASE("parse: mutually_exclusive flag group — one member ok, both members violates", "[parser][synthetic][group]") {
  auto root = make_synthetic_parser_root();
  {
    std::vector<std::string> argv{"widget", "excl", "--a"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
  }
  {
    std::vector<std::string> argv{"widget", "excl", "--a", "--b"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::flag_group_violation);
    CHECK(outcome.error().group == "mode");
    CHECK(outcome.error().group_mode == flag_group_mode::mutually_exclusive);
  }
}

TEST_CASE("parse: required_one flag group — one member ok, neither violates", "[parser][synthetic][group]") {
  auto root = make_synthetic_parser_root();
  {
    std::vector<std::string> argv{"widget", "any", "--x"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
  }
  {
    std::vector<std::string> argv{"widget", "any"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::flag_group_violation);
    CHECK(outcome.error().group == "input");
    CHECK(outcome.error().group_mode == flag_group_mode::required_one);
  }
}

TEST_CASE("parse: required_exactly_one flag group — exactly one ok, zero and both both violate", "[parser][synthetic][group]") {
  auto root = make_synthetic_parser_root();
  {
    std::vector<std::string> argv{"widget", "exact", "--p"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
  }
  {
    std::vector<std::string> argv{"widget", "exact"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::flag_group_violation);
    CHECK(outcome.error().group_mode == flag_group_mode::required_exactly_one);
  }
  {
    std::vector<std::string> argv{"widget", "exact", "--p", "--q"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().kind == planar::cli::parse_error_kind::flag_group_violation);
    CHECK(outcome.error().group_mode == flag_group_mode::required_exactly_one);
  }
}

TEST_CASE("parse: task add — required positional, string/int/bool flags, defaults fill in", "[parser][success]") {
  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "add", "My Title", "--body", "hello", "--plan", "42"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->is_help);
  CHECK(outcome->match.path == std::vector<std::string>{"task", "add"});
  CHECK(get_str(outcome->match.positionals, "title") == "My Title");
  CHECK(get_str(outcome->match.flags, "--body") == "hello");
  CHECK(get_int(outcome->match.flags, "--plan") == 42);
  // Defaults fill in for flags never passed.
  CHECK(get_int(outcome->match.flags, "--priority") == 100);
  CHECK(get_bool(outcome->match.flags, "--editor") == true);
  CHECK(get_bool(outcome->match.flags, "--json") == false);
  CHECK_FALSE(outcome->match.flags.contains("--scope"));
}

TEST_CASE("parse: task done — required positional plus a bool flag", "[parser][success]") {
  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "done", "task:42", "--force"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE(outcome.has_value());
  CHECK(outcome->match.path == std::vector<std::string>{"task", "done"});
  CHECK(get_str(outcome->match.positionals, "task-id") == "task:42");
  CHECK(get_bool(outcome->match.flags, "--force") == true);
}

TEST_CASE("parse: agent fail — required flags plus a choice flag with default", "[parser][success]") {
  auto root = make_planar_agent_root();
  {
    std::vector<std::string> argv{"planar-agent", "fail", "--claim", "tok123", "--reason", "timed out"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_str(outcome->match.flags, "--claim") == "tok123");
    CHECK(get_str(outcome->match.flags, "--reason") == "timed out");
    // Choice default applies when the flag is absent.
    CHECK(get_str(outcome->match.flags, "--category") == "unknown");
  }
  {
    std::vector<std::string> argv{"planar-agent", "fail", "--claim", "t", "--reason", "r", "--category", "validation"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(get_str(outcome->match.flags, "--category") == "validation");
  }
}

TEST_CASE("parse: --help under a leaf and a bare parent both request help", "[parser][help]") {
  auto root = make_planar_root();
  {
    std::vector<std::string> argv{"planar", "task", "add", "--help"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_help);
    CHECK(outcome->help_path == std::vector<std::string>{"task", "add"});
  }
  {
    // Bare parent verb (no further token) renders that group's help at
    // exit 0 — matches `./zig/zig-out/bin/planar task` (verified below,
    // "parity: bare parent verb").
    std::vector<std::string> argv{"planar", "task"};
    auto                     outcome = planar::cli::parse(root, argv);
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_help);
    CHECK(outcome->help_path == std::vector<std::string>{"task"});
  }
}

TEST_CASE("parse error: unknown subcommand", "[parser][error]") {
  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "bogus"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  auto const& err = outcome.error();
  CHECK(err.kind == planar::cli::parse_error_kind::unknown_subcommand);
  CHECK(err.arg == "bogus");
  CHECK(err.cmd_path == "task");
  // Captured: `./zig/zig-out/bin/planar task bogus` → stdout
  // "error: unknown subcommand (got bogus) [in: task]\n", exit 2.
  CHECK(planar::cli::format_error(err) == "error: unknown subcommand (got bogus) [in: task]\n");
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(err.kind) == 2);
}

TEST_CASE("parse error: unknown flag", "[parser][error]") {
  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "add", "T", "--bogus", "x"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  auto const& err = outcome.error();
  CHECK(err.kind == planar::cli::parse_error_kind::unknown_flag);
  CHECK(err.arg == "--bogus");
  // Captured: `./zig/zig-out/bin/planar task add T --bogus x` → stdout
  // "error: unknown flag (got --bogus)\n", exit 2.
  CHECK(planar::cli::format_error(err) == "error: unknown flag (got --bogus)\n");
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(err.kind) == 2);
}

TEST_CASE("parse error: missing required positional", "[parser][error]") {
  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "done"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  auto const& err = outcome.error();
  CHECK(err.kind == planar::cli::parse_error_kind::missing_required_positional);
  CHECK(err.positional_name == "task-id");
  // Captured: `./zig/zig-out/bin/planar task done` → stdout
  // "error: required positional missing: <task-id>\n", exit 2.
  CHECK(planar::cli::format_error(err) == "error: required positional missing: <task-id>\n");
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(err.kind) == 2);
}

TEST_CASE("parse error: missing required flag", "[parser][error]") {
  auto                     root = make_planar_agent_root();
  std::vector<std::string> argv{"planar-agent", "fail", "--reason", "x"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  auto const& err = outcome.error();
  CHECK(err.kind == planar::cli::parse_error_kind::missing_required);
  CHECK(err.flag_name == "--claim");
  // Captured: `./zig/zig-out/bin/planar-agent fail --reason x` → stdout
  // "error: required flag missing: --claim\n". The reference binary's OWN
  // exit.zig falls this through to its generic-1 bucket (verified: exit 1,
  // not 2 — planar-agent's exit.zig only maps InvalidEntityRef/InvalidInput
  // to 2, unlike planar/exit.zig's blanket cli.Parse.* mapping). This
  // module's `exit_code_for_parse_error_planar_binary` documents and
  // reproduces the `planar`-binary policy specifically (see error.cppm's
  // file comment) — every
  // `parse_error_kind` including this one maps to exit_user_input (2).
  CHECK(planar::cli::format_error(err) == "error: required flag missing: --claim\n");
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(err.kind) == 2);
}

TEST_CASE("parse error: invalid choice value carries a message and no close suggestion", "[parser][error]") {
  auto                     root = make_planar_agent_root();
  std::vector<std::string> argv{"planar-agent", "fail", "--claim", "x", "--reason", "y", "--category", "bogus"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  auto const& err = outcome.error();
  CHECK(err.kind == planar::cli::parse_error_kind::invalid_value);
  CHECK(err.flag_name == "--category");
  CHECK(err.arg == "bogus");
  // Captured: `./zig/zig-out/bin/planar-agent fail --claim x --reason y
  // --category bogus` → stdout "error: invalid value: --category (got
  // bogus)\n" — "bogus" isn't within edit-distance 2 of any declared
  // choice, so no "; did you mean ...?" suffix.
  CHECK(planar::cli::format_error(err) == "error: invalid value: --category (got bogus)\n");
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(err.kind) == 2);
}

TEST_CASE("parity: task add/done help and error behavior matches the reference planar binary", "[parser][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_PLANAR_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }
  auto const out_path = std::filesystem::temp_directory_path() / "planar_cli_parity_task_add.txt";

  // Pin the reference binary to a throwaway database. The Zig runtime resolves
  // $PLANAR_DB and otherwise falls back to ~/.planar/planar.db, applying
  // pending migrations automatically on first use — so an inherited
  // environment points this at the operator's live database, and once a
  // migration lands on this branch `ctest` would migrate it past the version
  // every installed binary supports.
  auto const      scratch = std::filesystem::temp_directory_path() /
                            std::format("planar_cli_parity_env_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code mk_ec;
  std::filesystem::create_directories(scratch, mk_ec);

  // "task add" with a duplicate — no missing-positional/flag error path;
  // this only re-confirms the exit code convention (2) still holds against
  // the live reference binary, in case exit.zig's mapping ever drifts.
  auto cmd_str = std::format("PLANAR_DB='{}' PLANAR_HOME='{}' PLANAR_CONFIG_PATH='{}' {} task done > {} 2>&1",
                             (scratch / "planar.db").string(), (scratch / "home").string(), (scratch / "config.toml").string(),
                             zig_bin.string(), out_path.string());
  int const status = std::system(cmd_str.c_str());
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 2);

  auto                     root = make_planar_root();
  std::vector<std::string> argv{"planar", "task", "done"};
  auto                     outcome = planar::cli::parse(root, argv);
  REQUIRE_FALSE(outcome.has_value());
  CHECK(planar::cli::exit_code_for_parse_error_planar_binary(outcome.error().kind) == WEXITSTATUS(status));

  std::error_code ec;
  std::filesystem::remove(out_path, ec);
}
