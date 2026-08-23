// @file args.t.cpp
// @brief Tests for `planar.cliapp.args` — the parsed-argv harvest, the
// accessors over it, and the ONE surviving implementation of Zig's
// `std.fmt.parseInt` semantics (plan 996, task 6123).
//
// ## What is actually at risk here
//
// Three things, none of them obvious from reading the module:
//
//   1. THE DEFAULTS. `CLI::Option::default_str` is HELP-ONLY metadata on
//      CLI11's side — for an option with no bound variable it never lands
//      in `results()`. `harvest` materializes it. Without that step every
//      handler reading `--ttl` or `--category` or `--vendor` would see
//      "absent" where the tree promised a value, the defaults would be
//      silently inert, and nothing would fail to compile.
//
//   2. THE INHERITED FLAGS. `harvest` walks the whole matched chain, not
//      just the leaf, so a flag declared on an ancestor reaches the
//      handler. A leaf-only harvest looks correct until someone declares a
//      root-level flag.
//
//   3. THE UNDERSCORE SEPARATORS. `1_0` is 10 on the reference binary and
//      `planar unlink 1_0` really does address link 10. `std::from_chars`
//      rejects it. This is the case a reasonable port drops.
//
// Before task 6123 the tree carried THREE copies of item 3 —
// `cli/parser.cpp`'s `numeric::normalize_int_token`, plus a verbatim
// `parse_int64_zig` in each of `cmd/planar/args.cppm` and
// `cmd/planar-agent/args.cppm`. It carries one now, and this is where it is
// tested at the unit level; `src/cmd/planar/handlers.t.cpp` additionally
// drives the whole table end-to-end through `planar unlink <arg>` against
// the live oracle, which is where the values were captured in the first
// place.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;

namespace {

using planar::cliapp::flag_bool;
using planar::cliapp::flag_int;
using planar::cliapp::flag_string;
using planar::cliapp::harvest;
using planar::cliapp::leaf_keys;
using planar::cliapp::parse_int64_zig;
using planar::cliapp::parsed_args;
using planar::cliapp::path_key;
using planar::cliapp::positional_int;
using planar::cliapp::positional_string;
using planar::cliapp::zig_int_validator;

/// @brief A root with an inherited flag, two defaulted options, an
/// int-validated option, a plain option and a nested positional.
/// @param app The root app to populate.
auto build_tree(CLI::App& app) -> void {
  app.require_subcommand(0);
  app.add_flag("--verbose");

  CLI::App* claim = app.add_subcommand("claim", "Claim it.");
  claim->add_option("--ttl")->default_str("600");
  claim->add_option("--vendor")->default_str("planar-agent");
  claim->add_option("--purpose");
  claim->add_option("--plan")->check(zig_int_validator());
  claim->add_flag("--json");

  CLI::App* action = app.add_subcommand("action", "Nested actions.");
  action->require_subcommand(0);
  CLI::App* start = action->add_subcommand("start", "Start one.");
  start->add_option("kind")->required();
}

/// @brief Parse `args` against a freshly built tree and harvest the result.
/// @param args The argv tail (argv[0] is not included, matching CLI11).
/// @return The harvested values.
auto run(std::vector<std::string> args) -> parsed_args {
  CLI::App app{"", "planar-agent"};
  build_tree(app);
  std::vector<std::string> reversed(args.rbegin(), args.rend());
  app.parse(std::move(reversed));
  return harvest(app);
}

} // namespace

// ---------------------------------------------------------------------------
// harvest
// ---------------------------------------------------------------------------

TEST_CASE("harvest records the matched subcommand path", "[cliapp][args][harvest]") {
  CHECK(run({"claim"}).path == std::vector<std::string>{"claim"});
  CHECK(run({"action", "start", "coder"}).path == std::vector<std::string>{"action", "start"});
  CHECK(run({}).path.empty());
}

TEST_CASE("harvest materializes a declared default for an absent flag", "[cliapp][args][harvest][defaults]") {
  // ITEM 1 from this file's header. CLI11 would have reported these as
  // absent; every claim verb's `--ttl 600` promise depends on this step.
  auto const args = run({"claim"});
  CHECK(flag_string(args, "--ttl") == "600");
  CHECK(flag_string(args, "--vendor") == "planar-agent");
  // An option with NO declared default stays genuinely absent.
  CHECK_FALSE(flag_string(args, "--purpose").has_value());
}

TEST_CASE("harvest prefers an argv value over the declared default", "[cliapp][args][harvest][defaults]") {
  auto const args = run({"claim", "--ttl", "8h"});
  CHECK(flag_string(args, "--ttl") == "8h");
}

TEST_CASE("harvest reaches flags declared on an ANCESTOR", "[cliapp][args][harvest][inherited]") {
  // ITEM 2. `--verbose` is declared on the root and supplied before the
  // subcommand; a leaf-only harvest would drop it.
  auto const args = run({"--verbose", "claim"});
  CHECK(flag_bool(args, "--verbose"));
  CHECK(args.path == std::vector<std::string>{"claim"});
}

TEST_CASE("harvest keeps a positional under its declared name", "[cliapp][args][harvest]") {
  auto const args = run({"action", "start", "coder"});
  CHECK(positional_string(args, "kind") == "coder");
}

TEST_CASE("harvest never records CLI11's auto-added --help flag", "[cliapp][args][harvest]") {
  auto const args = run({"claim"});
  CHECK_FALSE(args.flags.contains("--help"));
}

// ---------------------------------------------------------------------------
// accessors
// ---------------------------------------------------------------------------

TEST_CASE("flag_bool distinguishes present from absent, and honours the fallback", "[cliapp][args][accessors]") {
  CHECK(flag_bool(run({"claim", "--json"}), "--json"));
  CHECK_FALSE(flag_bool(run({"claim"}), "--json"));
  CHECK(flag_bool(run({"claim"}), "--json", true));
  CHECK_FALSE(flag_bool(run({"claim"}), "--json", false));
}

TEST_CASE("flag_string reports present-and-empty as present, not absent", "[cliapp][args][accessors]") {
  // The distinction is load-bearing at planar-agent's heartbeat call site:
  // `--status ""` clears the status column where an omitted `--status`
  // leaves it alone.
  auto const explicit_empty = run({"claim", "--purpose", ""});
  auto const omitted        = run({"claim"});
  CHECK(flag_string(explicit_empty, "--purpose") == "");
  CHECK_FALSE(flag_string(omitted, "--purpose").has_value());
}

TEST_CASE("flag_int and positional_int route through the Zig parser", "[cliapp][args][accessors]") {
  CHECK(flag_int(run({"claim", "--plan", "1_0"}), "--plan") == 10);
  CHECK(flag_int(run({"claim", "--plan", "+12"}), "--plan") == 12);
  CHECK_FALSE(flag_int(run({"claim"}), "--plan").has_value());
  CHECK(positional_int(run({"action", "start", "007"}), "kind") == 7);
  CHECK_FALSE(positional_int(run({"action", "start", "coder"}), "kind").has_value());
}

TEST_CASE("a repeated single-valued flag is REFUSED at parse time, not silently resolved",
          "[cliapp][args][accessors][duplicate]") {
  // Measured, not assumed — the first draft of this case asserted
  // last-wins and failed. CLI11 raises an ArgumentMismatch ("--purpose: At
  // most 1 required but received 2") rather than keeping either value.
  //
  // Worth pinning because it is the behaviour the DELETED parser also had
  // (etcli's `DuplicateFlag` was a parse error too), so the swap preserved
  // it by accident rather than by design — and because it is what makes
  // `flag_string`'s "read the last" implementation unobservable in practice
  // rather than a policy anyone relies on.
  CLI::App app{"", "planar-agent"};
  build_tree(app);
  std::vector<std::string> reversed{"second", "--purpose", "first", "--purpose", "claim"};
  CHECK_THROWS_AS(app.parse(std::move(reversed)), CLI::ArgumentMismatch);
}

TEST_CASE("every accessor is total: an unknown name yields the fallback, never a throw", "[cliapp][args][accessors]") {
  // The parser has ALREADY enforced required-ness, choice sets and value
  // kinds before a handler runs, so a miss here is a tree-authoring bug —
  // and the fallback keeps it from becoming a crash in an operator's shell.
  parsed_args const empty;
  CHECK_FALSE(flag_bool(empty, "--nope"));
  CHECK_FALSE(flag_string(empty, "--nope").has_value());
  CHECK_FALSE(flag_int(empty, "--nope").has_value());
  CHECK_FALSE(positional_string(empty, "nope").has_value());
  CHECK_FALSE(positional_int(empty, "nope").has_value());
}

// ---------------------------------------------------------------------------
// parse_int64_zig — ITEM 3
// ---------------------------------------------------------------------------

TEST_CASE("parse_int64_zig reproduces std.fmt.parseInt, separators included", "[cliapp][args][parity]") {
  // Every one of these was captured through `planar unlink <arg>` against
  // the oracle; the not-found message interpolates the PARSED value, so a
  // divergence is operator-visible.
  CHECK(parse_int64_zig("12") == 12);
  CHECK(parse_int64_zig("+12") == 12);
  CHECK(parse_int64_zig("-5") == -5);
  CHECK(parse_int64_zig("007") == 7);
  CHECK(parse_int64_zig("0") == 0);
  CHECK(parse_int64_zig("1_0") == 10);
  CHECK(parse_int64_zig("1__0") == 10);
  CHECK(parse_int64_zig("1_2_3") == 123);

  CHECK_FALSE(parse_int64_zig("_10").has_value());
  CHECK_FALSE(parse_int64_zig("10_").has_value());
  CHECK_FALSE(parse_int64_zig("+_1").has_value());
  CHECK_FALSE(parse_int64_zig(" 12").has_value());
  CHECK_FALSE(parse_int64_zig("12 ").has_value());
  CHECK_FALSE(parse_int64_zig("0x10").has_value());
  CHECK_FALSE(parse_int64_zig("12abc").has_value());
  CHECK_FALSE(parse_int64_zig("").has_value());
  CHECK_FALSE(parse_int64_zig("+").has_value());
  CHECK_FALSE(parse_int64_zig("-").has_value());
}

TEST_CASE("parse_int64_zig is exact at the i64 boundary", "[cliapp][args][parity]") {
  CHECK(parse_int64_zig("9223372036854775807") == std::numeric_limits<std::int64_t>::max());
  CHECK_FALSE(parse_int64_zig("9223372036854775808").has_value());
  CHECK(parse_int64_zig("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("zig_int_validator refuses at PARSE time what parse_int64_zig refuses", "[cliapp][args][parity][validator]") {
  // Without the validator an invalid integer would degrade to a silent
  // `std::nullopt` at the handler — indistinguishable from an omitted
  // flag, which is a real behaviour regression against the oracle's
  // parse-time refusal. This asserts the two agree on both directions.
  auto const parses = [](std::string_view value) {
    CLI::App app{"", "tool"};
    app.add_option("--plan")->check(zig_int_validator());
    std::vector<std::string> reversed{std::string{value}, "--plan"};
    try {
      app.parse(std::move(reversed));
      return true;
    } catch (const CLI::ParseError&) {
      return false;
    }
  };
  for (auto const& good : {"12", "+12", "007", "1_0", "1__0"}) {
    INFO("should parse: " << good);
    CHECK(parses(good));
    CHECK(parse_int64_zig(good).has_value());
  }
  for (auto const& bad : {"_10", "10_", "0x10", "12abc", "9223372036854775808"}) {
    INFO("should refuse: " << bad);
    CHECK_FALSE(parses(bad));
    CHECK_FALSE(parse_int64_zig(bad).has_value());
  }
}

// ---------------------------------------------------------------------------
// path_key / leaf_keys — the dispatch registration gate's input
// ---------------------------------------------------------------------------

TEST_CASE("path_key joins a resolved path with single spaces", "[cliapp][args][dispatch]") {
  CHECK(path_key(std::vector<std::string>{"action", "start"}) == "action start");
  CHECK(path_key(std::vector<std::string>{"claim"}) == "claim");
  CHECK(path_key(std::vector<std::string>{}).empty());
}

TEST_CASE("leaf_keys reports childless nodes only, at every depth", "[cliapp][args][dispatch]") {
  // This is what each binary's `unregistered_leaves` gate walks. Reporting
  // a PARENT as a leaf would demand a handler for `action`; missing a
  // nested leaf would let `action start` ship unwired.
  CLI::App app{"", "planar-agent"};
  build_tree(app);
  auto keys = leaf_keys(app);
  std::ranges::sort(keys);
  CHECK(keys == std::vector<std::string>{"action start", "claim"});
}
