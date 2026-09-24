// @file manifest.t.cpp
// @brief The pure half of `planar.engine_execute`: the frozen manifest's
// shape, the two allowlists, the path/ref guards, and the float formatter
// (plan 996, task 6042).
//
// Everything here runs without a `lua_State` and without spawning anything.
// The LIVE half — what a workflow can actually reach in a real state — is
// `surface.t.cpp`, and the two are deliberately separate: this file could
// pass in full while the registrar installed something else entirely.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

namespace {

namespace ex = planar::engine::execute;

/// @brief Convenience: `command_allowed` over a literal argv.
/// @param bin The binary name.
/// @param argv The arguments.
/// @return Whether the command is allowed.
auto allowed(std::string_view bin, std::vector<std::string> const& argv) -> bool {
  return ex::command_allowed(bin, argv);
}

} // namespace

TEST_CASE("the host manifest is twenty-five entries across exactly five tables", "[engine][execute][manifest]") {
  auto const manifest = ex::allowed_host_fns();
  // The COUNT is asserted on its own, not just the contents. A test that only
  // compared the contents against the same table it is defined in would pass
  // trivially after an edit to both; a stated number is a second,
  // independent fact about the surface, and it is the one a reviewer reads.
  CHECK(manifest.size() == 25);

  std::map<std::string_view, int> per_table;
  for (auto const& entry : manifest) {
    ++per_table[entry.table];
  }
  CHECK(per_table.size() == 5);
  CHECK(per_table["cli"] == 6);
  CHECK(per_table["git"] == 5);
  CHECK(per_table["fs"] == 4);
  CHECK(per_table["flow"] == 4);
  CHECK(per_table["ctx"] == 6);

  // No duplicates: two entries with the same (table, name) would register
  // twice and the live enumeration would still see one, hiding the mistake.
  std::set<std::pair<std::string_view, std::string_view>> unique;
  for (auto const& entry : manifest) {
    unique.emplace(entry.table, entry.name);
  }
  CHECK(unique.size() == manifest.size());
}

TEST_CASE("no manifest entry names a denied spawn primitive", "[engine][execute][manifest]") {
  // `manifest.cpp` already carries a `static_assert` for this, so a violation
  // does not compile. The runtime check is here anyway because the two
  // statements are not the same one: the static_assert covers the table as
  // written, this covers the table as EXPOSED through `allowed_host_fns()`,
  // and a future accessor that filtered or appended would break the second
  // without touching the first.
  auto const denied = ex::denied_host_fns();
  for (auto const& entry : ex::allowed_host_fns()) {
    INFO("host fn " << entry.table << "." << entry.name);
    CHECK(std::ranges::find(denied, entry.name) == denied.end());
  }
  // And the denylist actually names the things it is supposed to name — a
  // denylist that quietly emptied would pass the loop above vacuously.
  for (auto const& name : {"agent", "exec", "spawn", "claude", "codex", "model", "headless"}) {
    INFO("denied name " << name);
    CHECK(std::ranges::find(denied, std::string_view{name}) != denied.end());
  }
}

TEST_CASE("only three Planar binaries may be shelled", "[engine][execute][manifest]") {
  auto const bins = ex::allowed_cli_bins();
  REQUIRE(bins.size() == 3);
  CHECK(std::ranges::find(bins, std::string_view{"planar"}) != bins.end());
  CHECK(std::ranges::find(bins, std::string_view{"planar-agent"}) != bins.end());
  CHECK(std::ranges::find(bins, std::string_view{"planar-watch"}) != bins.end());
}

TEST_CASE("the command allowlist gates verbs, not just binaries", "[engine][execute][manifest]") {
  // Being allowed to shell `planar` is NOT being allowed to run every planar
  // verb. This is the second gate, and it is the one that keeps the terminal
  // task-status surface out of a workflow's reach.
  CHECK(allowed("planar", {"plan", "show", "1", "--json"}));
  CHECK(allowed("planar", {"task", "show", "1"}));
  CHECK(allowed("planar", {"schema"}));
  CHECK(allowed("planar", {"health"}));

  // `planar task done` is the specific thing a one-token match would have let
  // through on the strength of `planar task show` being allowed. Claim
  // termination goes through planar-agent's atomic verbs or not at all.
  CHECK_FALSE(allowed("planar", {"task", "done", "1"}));
  CHECK_FALSE(allowed("planar", {"task", "update", "1", "--status", "done"}));
  CHECK_FALSE(allowed("planar", {"init"}));
  CHECK_FALSE(allowed("planar", {"sync", "push"}));
  CHECK_FALSE(allowed("planar", {"task"})); // A verb with no subcommand.
  CHECK_FALSE(allowed("planar", {}));       // No arguments at all.

  CHECK(allowed("planar-agent", {"heartbeat", "--claim", "x"}));
  CHECK(allowed("planar-agent", {"complete"}));
  CHECK_FALSE(allowed("planar-agent", {"nope"}));

  CHECK(allowed("planar-watch", {"version"}));
  CHECK_FALSE(allowed("planar-watch", {"nope"}));

  // A binary outside the three is refused regardless of its arguments —
  // including `git`, which has its own confined group and must never be
  // reachable through the generic CLI shell.
  CHECK_FALSE(allowed("git", {"status"}));
  CHECK_FALSE(allowed("sh", {"-c", "echo hi"}));
  CHECK_FALSE(allowed("claude", {"--print"}));
}

TEST_CASE("git arguments are validated before they reach git", "[engine][execute][manifest]") {
  CHECK(ex::is_hex_object_id("abc1234"));
  CHECK(ex::is_hex_object_id("55d8c00a8e876edaf31ce30a4dd7df444d334100"));
  CHECK_FALSE(ex::is_hex_object_id("abc123"));  // Six digits: too short.
  CHECK_FALSE(ex::is_hex_object_id("zzzzzzz")); // Right length, not hex.
  CHECK_FALSE(ex::is_hex_object_id(""));
  CHECK_FALSE(ex::is_hex_object_id(std::string(65, 'a')));

  CHECK(ex::safe_git_ref("main"));
  CHECK(ex::safe_git_ref("HEAD~1"));
  CHECK(ex::safe_git_ref("refs/heads/feature/x"));
  // A ref beginning `-` is the whole reason this guard exists: git would
  // read it as an option, and `--upload-pack=...` is a remote-code-execution
  // shape.
  CHECK_FALSE(ex::safe_git_ref("--upload-pack=evil"));
  CHECK_FALSE(ex::safe_git_ref("-x"));
  CHECK_FALSE(ex::safe_git_ref("a..b")); // Range syntax, and `..` generally.
  CHECK_FALSE(ex::safe_git_ref("a b"));
  CHECK_FALSE(ex::safe_git_ref("a;rm -rf /"));
  CHECK_FALSE(ex::safe_git_ref(""));
}

TEST_CASE("sandbox-relative paths reject every textual escape", "[engine][execute][manifest]") {
  CHECK(ex::validate_confined_rel("a.txt"));
  CHECK(ex::validate_confined_rel("sub/dir/a.txt"));

  CHECK_FALSE(ex::validate_confined_rel(""));
  CHECK_FALSE(ex::validate_confined_rel("/etc/passwd"));
  CHECK_FALSE(ex::validate_confined_rel("../a.txt"));
  CHECK_FALSE(ex::validate_confined_rel("sub/../../a.txt"));
  CHECK_FALSE(ex::validate_confined_rel("./a.txt"));
  CHECK_FALSE(ex::validate_confined_rel("sub//a.txt"));
  CHECK_FALSE(ex::validate_confined_rel("sub\\a.txt"));
  CHECK_FALSE(ex::validate_confined_rel("a/"));
}

TEST_CASE("floats format the way the oracle prints them, not the way to_chars does", "[engine][execute][manifest]") {
  // Every expected string below was produced by RUNNING
  // zig/zig-out/bin/planar-execute over a workflow returning these values,
  // not derived from a formatting rule.
  //
  // The two that matter are `1e300` and `3e-7`. Zig's `{d}` is
  // shortest-round-trip digits WITHOUT an exponent, which is neither what
  // `std::format("{}", d)` gives (`1e+300`) nor what
  // `to_chars(chars_format::fixed)` gives (the exact binary value —
  // `1000000000000000052504760255204420248704…`, three hundred digits of
  // noise). A first implementation used `fixed` and this case is what caught
  // it.
  CHECK(ex::format_double(1.0) == "1");
  CHECK(ex::format_double(100.0) == "100");
  CHECK(ex::format_double(-0.0) == "-0");
  CHECK(ex::format_double(0.1) == "0.1");
  CHECK(ex::format_double(1.0 / 3.0) == "0.3333333333333333");
  CHECK(ex::format_double(3.0e-7) == "0.0000003");
  CHECK(ex::format_double(-1.5e-10) == "-0.00000000015");
  CHECK(ex::format_double(1e21) == std::string("1") + std::string(21, '0'));
  CHECK(ex::format_double(1e300) == std::string("1") + std::string(300, '0'));
  CHECK(ex::format_double(9007199254740992.0) == "9007199254740992");

  // Non-finite values keep Zig's spellings. None of the three is legal JSON
  // and the oracle emits them anyway; substituting `null` would make a
  // workflow that divided by zero indistinguishable from one that returned
  // nothing.
  CHECK(ex::format_double(std::numeric_limits<double>::infinity()) == "inf");
  CHECK(ex::format_double(-std::numeric_limits<double>::infinity()) == "-inf");
  CHECK(ex::format_double(std::numeric_limits<double>::quiet_NaN()) == "nan");
}
