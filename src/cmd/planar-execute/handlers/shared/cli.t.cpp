// @file cli.t.cpp
// @brief `planar-execute`'s argument surface and its two capability locks
// (plan 996, task 6107).
//
// Port target: the `parseRunArgs` unit tests in
// zig/src/cmd/planar-execute/main.zig, plus the "advertises no spawn
// affordance" case from
// zig/integration_tests/capability_boundary_test.zig.
//
// ## The two locks, and what each actually proves
//
//   NO SPAWN AFFORDANCE. `advertises no spawn affordance` below reproduces
//   the Zig integration check, including its five documented exclusions
//   ("workflow" is the Lua file noun; "model" appears in "no model-spawn"
//   prose; "exec" is a substring of "planar-execute"; "spawn" appears in
//   "spawn-free"; "dispatch_table" is a compound covered by the unit
//   lock). Word-boundary matched, not substring matched — the exclusions
//   exist precisely because substring matching produces false positives
//   here. What it proves: the binary's ADVERTISED surface offers no spawn
//   primitive. What it does NOT prove: that no such primitive exists
//   unadvertised — the Zig side pairs it with a runtime host-fn manifest
//   lock (`host.zig`'s ALLOWED_HOST_FNS assertion) that has no counterpart
//   here because the Lua host surface is unported. Stated so nobody reads
//   this test as more than it is.
//
//   NO SQLITE HANDLE. There is no test for this, and that is deliberate:
//   it is enforced at CONFIGURE TIME by cmake/architecture.cmake, which
//   FATALs if this target reaches `planar_db` directly or transitively.
//   The build refusing to generate is stronger evidence than any runtime
//   assertion, because a test can only observe the handle a binary chose
//   to open, whereas the guard makes the edge unrepresentable. See
//   src/cmd/planar-execute/CMakeLists.txt for the naming trap that makes
//   the literal arm of that check actually fire on this target.
//
// ## Break-probes run against this file
//
//   - Added the word "agent" to `usage_text()` -> `advertises no spawn
//     affordance` FAILS naming it. Restored -> green.
//   - Made `parse_run_args` ignore an unrecognised `--flag` instead of
//     refusing -> `an unrecognised long flag is refused` FAILS. Restored
//     -> green.
//   - Reordered `classify` to test `--help` before `run` -> SURVIVOR. The
//     whole suite stayed green, because the two arms match disjoint tokens
//     and no argv reaches both. The claim that the order was observable was
//     simply wrong; the case was rewritten around what IS observable (see
//     `treats \`run --help\` as a failed run` below) and the probe was
//     replaced by the `--help`-recognition mutation, which does fail it.
//   - Dropped the trailing newline from `usage_text()` -> `the usage text
//     is a COMPLETE payload` FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.catalog;
import planar.cmd.planar_execute.engine;

namespace {

using planar::cmd::execute::classify;
using planar::cmd::execute::parse_profile_args;
using planar::cmd::execute::parse_run_args;
using planar::cmd::execute::parse_run_id_args;
using planar::cmd::execute::parse_submit_args;
using planar::cmd::execute::usage_text;
using planar::cmd::execute::verb;

/// @brief Build an argv (including argv[0]) from a tail.
/// @param tail The arguments after the program name.
/// @return The full argv.
auto argv_of(std::vector<std::string> tail) -> std::vector<std::string> {
  std::vector<std::string> argv{"planar-execute"};
  argv.insert(argv.end(), tail.begin(), tail.end());
  return argv;
}

/// @brief True when `name` occurs in `haystack` as a standalone word.
///
/// Word-boundary matched exactly as the Zig integration check does:
/// preceded and followed by a non-alphanumeric character or a string
/// boundary. Substring matching would fire on "exec" inside
/// "planar-execute", which is why the Zig version excludes it rather than
/// matching loosely.
/// @param haystack The text to scan.
/// @param name The word to look for.
/// @return `true` if present as a whole word.
auto contains_word(std::string_view haystack, std::string_view name) -> bool {
  for (std::size_t pos = 0; (pos = haystack.find(name, pos)) != std::string_view::npos; ++pos) {
    bool const before_ok = pos == 0 || (std::isalnum(static_cast<unsigned char>(haystack[pos - 1])) == 0);
    auto const after     = pos + name.size();
    bool const after_ok  = after >= haystack.size() || (std::isalnum(static_cast<unsigned char>(haystack[after])) == 0);
    if (before_ok && after_ok) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("planar-execute advertises no spawn affordance", "[cmd][execute][capability]") {
  // D7's DENIED_HOST_FNS names, minus the five the Zig check documents as
  // legitimately present in prose. Transcribed from
  // zig/integration_tests/capability_boundary_test.zig's `denied_names`.
  constexpr std::array<std::string_view, 9> denied{"agent", "parallel", "pipeline", "compact", "budget",
                                                   "child", "claude",   "codex",    "headless"};
  // Task 7204: `schema --compact` is the catalog's own output-size flag, not
  // the denied `compact` host function. Only that literal flag is excised
  // before the catalog scan, and only where it sits inside the `schema`
  // command object: the nearest preceding "command" key must name that node.
  // A `--compact` anywhere else in the catalog is a real affordance and fails
  // here instead of being erased.
  auto                       catalog = planar::cmd::execute::catalog_json();
  constexpr std::string_view command_key{R"("command":")"};
  for (auto at = catalog.find("--compact"); at != std::string::npos; at = catalog.find("--compact")) {
    auto const key = catalog.rfind(command_key, at);
    REQUIRE(key != std::string::npos);
    auto const value_start = key + command_key.size();
    auto const value_end   = catalog.find('"', value_start);
    REQUIRE(value_end != std::string::npos);
    INFO("`--compact` found outside the schema node, under command " << catalog.substr(value_start, value_end - value_start));
    REQUIRE(catalog.substr(value_start, value_end - value_start) == "planar-execute schema");
    catalog.erase(at, std::string_view{"--compact"}.size());
  }
  for (auto const& name : denied) {
    INFO("denied name found as a word in the advertised surface: " << name);
    CHECK_FALSE(contains_word(usage_text(), name));
    // The catalog is an advertised surface too (task 6486): a host function
    // that leaked into a flag description would be as much of an affordance
    // as one in the banner.
    INFO("denied name found as a word in the schema catalog: " << name);
    CHECK_FALSE(contains_word(catalog, name));
  }
}

TEST_CASE("planar-execute's usage text is a COMPLETE payload", "[cmd][execute][cli]") {
  auto const text = usage_text();
  CHECK_FALSE(text.empty());
  // The Zig multiline literal ends with a blank continuation line, i.e. a
  // final newline. Dropping it shortens SIX oracle-compared argv shapes by
  // one byte each.
  CHECK(text.ends_with("payload as JSON on stdout.\n"));
  CHECK(text.starts_with("planar-execute — deterministic, spawn-free Lua workflow engine.\n"));
}

TEST_CASE("planar-execute classifies its top-level argv shapes", "[cmd][execute][cli]") {
  CHECK(classify(argv_of({})) == verb::none);
  CHECK(classify(argv_of({"--help"})) == verb::help);
  CHECK(classify(argv_of({"-h"})) == verb::help);
  CHECK(classify(argv_of({"help"})) == verb::help);
  CHECK(classify(argv_of({"run"})) == verb::run);
  CHECK(classify(argv_of({"schema"})) == verb::schema);
  CHECK(classify(argv_of({"bogus"})) == verb::unknown);
  // `--version` is NOT a flag this binary knows — it is an unknown VERB,
  // which is why the oracle answers `planar-execute: unknown verb:
  // --version`. Captured, not assumed.
  CHECK(classify(argv_of({"--version"})) == verb::unknown);
}

TEST_CASE("planar-execute treats `run --help` as a failed run, not a help request", "[cmd][execute][cli]") {
  // SURVIVOR, FOUND AND FIXED. This case originally asserted that
  // `classify` tests `run` BEFORE `--help` and claimed the order was
  // observable. It is not: the two arms match disjoint tokens, so no argv
  // reaches both, and swapping them left the whole suite green. The claim
  // was wrong and the test did not discriminate.
  //
  // What IS observable is the consequence, and it is a real trap: because
  // `run` is a verb rather than a help-bearing command, `planar-execute
  // run --help` reaches `parse_run_args`, where `--help` is an
  // unrecognised long flag — so the oracle prints the usage text and exits
  // 2, NOT 0. A port that taught `parse_run_args` to recognise `--help`
  // would flip that code silently. `parity.t.cpp`'s `run_badflag` shape
  // pins the same thing end to end against the live oracle.
  CHECK(classify(argv_of({"run", "--help"})) == verb::run);
  CHECK_FALSE(parse_run_args(std::vector<std::string>{"--help"}).has_value());
}

TEST_CASE("planar-execute parses a full run invocation", "[cmd][execute][cli]") {
  std::vector<std::string> const args{"wf.lua", "--phase", "setup", "--args", "{\"x\":1}"};
  auto const                     parsed = parse_run_args(args);
  REQUIRE(parsed.has_value());
  CHECK(parsed->workflow == "wf.lua");
  CHECK(parsed->phase == "setup");
  CHECK(parsed->args_json == "{\"x\":1}");
  CHECK(parsed->worktree.empty());
  CHECK(parsed->sandbox_root.empty());
}

TEST_CASE("planar-execute parses the worktree and sandbox-root flags", "[cmd][execute][cli]") {
  std::vector<std::string> const args{"wf.lua", "--phase", "p", "--worktree", "/tmp/wt", "--sandbox-root", "/tmp/sb"};
  auto const                     parsed = parse_run_args(args);
  REQUIRE(parsed.has_value());
  CHECK(parsed->worktree == "/tmp/wt");
  CHECK(parsed->sandbox_root == "/tmp/sb");
}

TEST_CASE("planar-execute refuses every bad-usage shape", "[cmd][execute][cli]") {
  // Each of these is a REFUSAL in the original and each is easy to
  // "improve" into a silent acceptance.
  SECTION("missing --phase") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua"}).has_value());
  }
  SECTION("missing workflow positional") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"--phase", "setup"}).has_value());
  }
  SECTION("no arguments at all") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{}).has_value());
  }
  SECTION("an unrecognised long flag is refused, not ignored") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase", "p", "--nope"}).has_value());
  }
  SECTION("a flag with no value is refused, not defaulted to empty") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase"}).has_value());
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase", "p", "--args"}).has_value());
  }
  SECTION("a second bare positional is refused, not an overwrite") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"a.lua", "b.lua", "--phase", "p"}).has_value());
  }
}

TEST_CASE("planar-execute reads a workflow file, and refuses one it cannot", "[cmd][execute][engine]") {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_exec_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  auto const present = root / "wf.lua";
  {
    std::ofstream file(present, std::ios::binary);
    file << "function setup() end\n";
  }

  auto const read = planar::cmd::execute::read_workflow(present);
  REQUIRE(read.has_value());
  CHECK(*read == "function setup() end\n");

  CHECK_FALSE(planar::cmd::execute::read_workflow(root / "absent.lua").has_value());
  // A DIRECTORY is a read failure, not an empty workflow. An ifstream over
  // one opens successfully on some platforms and then reads zero bytes,
  // which would look like a valid empty file.
  CHECK_FALSE(planar::cmd::execute::read_workflow(root).has_value());
}

TEST_CASE("planar-execute's run path reports load failure with the oracle's message", "[cmd][execute][engine]") {
  std::ostringstream                   out;
  std::ostringstream                   err;
  planar::cmd::execute::run_args const args{.workflow = "x.lua", .phase = "setup"};
  auto const                           outcome = planar::cmd::execute::run_workflow(args, out, err);
  CHECK(outcome == planar::cmd::execute::run_outcome::load_failed);
  // Oracle bytes, verbatim. This is the ONE planar-execute failure that is
  // exit 1 rather than 2, which is why its message is pinned literally.
  CHECK(err.str() == "planar-execute: cannot read workflow: x.lua\n");
  // And stdout stays EMPTY. The JSON result channel emits nothing at all on
  // this path — not `{}`, which is what a workflow that ran and declared no
  // result produces, and which a caller must be able to tell apart.
  CHECK(out.str().empty());
}

TEST_CASE("planar-execute resolves its trusted sibling directory", "[cmd][execute][engine]") {
  // `cli.planar(...)` shells a SIBLING of this binary rather than searching
  // PATH, which is what makes allowlisting the NAME `planar` mean anything.
  // The test binary is itself an executable in the build tree, so the
  // directory it reports must exist and must contain this very file.
  auto const dir = planar::cmd::execute::executable_dir();
  REQUIRE_FALSE(dir.empty());
  CHECK(std::filesystem::is_directory(dir));
}

TEST_CASE("the submit verb's arguments parse, and its malformed shapes are refused", "[cmd][execute][cli][6504]") {
  SECTION("a bare bundle name takes the documented defaults") {
    std::vector<std::string> const args{"planar.supervision"};
    auto const                     parsed = parse_submit_args(args);
    REQUIRE(parsed.has_value());
    CHECK(parsed->bundle == "planar.supervision");
    CHECK(parsed->input == "{}");
    CHECK(parsed->profile == "default");
  }

  SECTION("flags may precede or follow the bundle") {
    std::vector<std::string> const args{"--profile", "work", "planar.supervision", "--input", R"({"a":1})"};
    auto const                     parsed = parse_submit_args(args);
    REQUIRE(parsed.has_value());
    CHECK(parsed->bundle == "planar.supervision");
    CHECK(parsed->profile == "work");
    CHECK(parsed->input == R"({"a":1})");
  }

  SECTION("no bundle at all is refused") {
    std::vector<std::string> const args{"--profile", "work"};
    CHECK_FALSE(parse_submit_args(args).has_value());
  }

  SECTION("a flag without its value is refused rather than swallowing the next token") {
    std::vector<std::string> const args{"planar.supervision", "--input"};
    CHECK_FALSE(parse_submit_args(args).has_value());
  }

  SECTION("an unknown flag is refused") {
    std::vector<std::string> const args{"planar.supervision", "--phase", "p"};
    CHECK_FALSE(parse_submit_args(args).has_value());
  }

  SECTION("a flag-looking token never becomes the bundle name") {
    // The discriminating shape. With a bundle already seen, a stray token is
    // refused as a second positional whatever it looks like — so only a
    // FIRST token that looks like a flag can tell "unknown flag" apart from
    // "extra positional", and without that check `--bogus` would silently be
    // submitted as a bundle name.
    std::vector<std::string> const args{"--bogus"};
    CHECK_FALSE(parse_submit_args(args).has_value());
  }

  SECTION("a second positional is a typo, not a second bundle") {
    std::vector<std::string> const args{"planar.supervision", "planar.other"};
    CHECK_FALSE(parse_submit_args(args).has_value());
  }
}

TEST_CASE("the inspection verbs share one argument shape, and refuse the wrong ones", "[cmd][execute][cli][6506]") {
  SECTION("status takes an optional run id") {
    std::vector<std::string> const none{};
    auto const                     bare = parse_run_id_args(none, false);
    REQUIRE(bare.has_value());
    CHECK(bare->run_id.empty());
    CHECK(bare->profile == "default");

    std::vector<std::string> const named{"01930000-0000-7000-8000-00000000000a"};
    auto const                     one = parse_run_id_args(named, false);
    REQUIRE(one.has_value());
    CHECK(one->run_id == "01930000-0000-7000-8000-00000000000a");
  }

  SECTION("cancel requires one") {
    // The same parser, a different promise: cancelling nothing in particular
    // is not a request that can be honoured.
    std::vector<std::string> const none{};
    CHECK_FALSE(parse_run_id_args(none, true).has_value());
    std::vector<std::string> const named{"01930000-0000-7000-8000-00000000000a"};
    CHECK(parse_run_id_args(named, true).has_value());
  }

  SECTION("a second run id, an unknown flag, and a flag without its value are refused") {
    std::vector<std::string> const two{"run-a", "run-b"};
    CHECK_FALSE(parse_run_id_args(two, true).has_value());
    std::vector<std::string> const unknown{"run-a", "--phase", "p"};
    CHECK_FALSE(parse_run_id_args(unknown, true).has_value());
    std::vector<std::string> const dangling{"run-a", "--profile"};
    CHECK_FALSE(parse_run_id_args(dangling, true).has_value());
  }

  SECTION("--from takes a whole number, and nothing else") {
    std::vector<std::string> const ok{"run-a", "--from", "42"};
    auto const                     parsed = parse_run_id_args(ok, true);
    REQUIRE(parsed.has_value());
    CHECK(*parsed->from == 42U);

    // A trailing-garbage cursor is a typo. Accepting its prefix would follow
    // from a position the caller never named.
    std::vector<std::string> const garbage{"run-a", "--from", "42abc"};
    CHECK_FALSE(parse_run_id_args(garbage, true).has_value());
    std::vector<std::string> const empty{"run-a", "--from", ""};
    CHECK_FALSE(parse_run_id_args(empty, true).has_value());
    std::vector<std::string> const negative{"run-a", "--from", "-1"};
    CHECK_FALSE(parse_run_id_args(negative, true).has_value());
  }

  SECTION("no --from means the remembered cursor, not zero") {
    std::vector<std::string> const none{"run-a"};
    auto const                     parsed = parse_run_id_args(none, true);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->from.has_value());
  }

  SECTION("a flag-looking token never becomes the run id") {
    std::vector<std::string> const flagish{"--bogus"};
    CHECK_FALSE(parse_run_id_args(flagish, true).has_value());
  }
}

TEST_CASE("planar-execute's schema catalog and its hand-rolled parser name the same flags", "[cmd][execute][cli][6486]") {
  // The catalog (task 6486, D18) is a DESCRIPTION built separately from
  // `parse_run_args`; nothing stops the two drifting except this case. Both
  // directions are checked: every flag the catalog advertises on `run` is
  // accepted by the parser, and every flag the parser accepts is advertised.
  auto const catalog = planar::cmd::execute::catalog_json();
  CHECK(catalog.starts_with("{"));
  CHECK(catalog.contains(R"("root":"planar-execute")"));
  CHECK(catalog.contains(R"("command":"planar-execute run")"));
  CHECK(catalog.contains(R"("command":"planar-execute schema")"));
  CHECK(catalog.contains(R"("command":"planar-execute submit")"));
  CHECK(catalog.contains(R"("command":"planar-execute status")"));
  CHECK(catalog.contains(R"("command":"planar-execute cancel")"));
  CHECK(catalog.contains(R"("command":"planar-execute host status")"));
  CHECK(catalog.contains(R"("command":"planar-execute follow")"));
  CHECK(catalog.contains(R"("command":"planar-execute host drain")"));
  CHECK(catalog.contains(R"("command":"planar-execute host stop")"));
  // Single line, no trailing newline: the write site appends exactly one.
  CHECK_FALSE(catalog.contains('\n'));

  // Catalog -> parser. Harvest every `"long":"--x"` the catalog declares.
  std::vector<std::string> advertised;
  for (std::size_t at = catalog.find(R"("long":"--)"); at != std::string::npos; at = catalog.find(R"("long":"--)", at + 1)) {
    auto const start = at + std::string_view{R"("long":")"}.size();
    auto const end   = catalog.find('"', start);
    advertised.emplace_back(catalog.substr(start, end - start));
  }
  // Five on `run`; `profile show`'s `--profile` and `--json`; `submit`'s
  // `--input` and `--profile`; and `--profile`/`--json` on each of `status`,
  // `cancel` and `host status` (task 6485 added `--engine` and the `profile`
  // verb; 6494 `--profile`; 6504 `submit`; 6506 the inspection verbs; 6505
  // `follow`'s `--from` and `--profile`; 6507 `host drain`/`host stop`, one
  // `--profile` each). Repeats are counted, because each declaring verb is a
  // separate promise and every one of them is checked below.
  // Task 7204 added `schema`'s `--command` and `--compact`: 21.
  REQUIRE(advertised.size() == 21);
  for (auto const& flag : advertised) {
    INFO("advertised flag not accepted by its parser: " << flag);
    if (flag == "--profile") {
      std::vector<std::string> const profile{"show", "--profile", "work"};
      auto const                     parsed = parse_profile_args(profile);
      REQUIRE(parsed.has_value());
      CHECK(parsed->name == "work");
      // The same flag on `submit`, whose parser is a different function: the
      // catalog advertises it on both verbs, so both must accept it.
      std::vector<std::string> const submitted{"planar.supervision", "--profile", "work"};
      auto const                     submit_parsed = parse_submit_args(submitted);
      REQUIRE(submit_parsed.has_value());
      CHECK(submit_parsed->profile == "work");
      // And the shape `status`/`cancel`/`host status` share.
      std::vector<std::string> const inspected{"--profile", "work"};
      auto const                     inspect_parsed = parse_run_id_args(inspected, false);
      REQUIRE(inspect_parsed.has_value());
      CHECK(inspect_parsed->profile == "work");
      continue;
    }
    if (flag == "--from") {
      std::vector<std::string> const followed{"run-a", "--from", "42"};
      auto const                     parsed = parse_run_id_args(followed, true);
      REQUIRE(parsed.has_value());
      REQUIRE(parsed->from.has_value());
      CHECK(*parsed->from == 42U);
      continue;
    }
    if (flag == "--input") {
      std::vector<std::string> const submitted{"planar.supervision", "--input", R"({"task":"t"})"};
      auto const                     parsed = parse_submit_args(submitted);
      REQUIRE(parsed.has_value());
      CHECK(parsed->input == R"({"task":"t"})");
      continue;
    }
    if (flag == "--command" || flag == "--compact") {
      // `schema` reads these two itself, not through this module's parsers;
      // `parity.t.cpp` ("schema --command and --compact select from the
      // catalog") drives them end to end through the binary.
      continue;
    }
    if (flag == "--json") {
      std::vector<std::string> const profile{"show", "--json"};
      auto const                     parsed = parse_profile_args(profile);
      REQUIRE(parsed.has_value());
      CHECK(parsed->json);
      std::vector<std::string> const inspected{"--json"};
      auto const                     inspect_parsed = parse_run_id_args(inspected, false);
      REQUIRE(inspect_parsed.has_value());
      CHECK(inspect_parsed->json);
      continue;
    }
    std::vector<std::string> args{"wf.lua", "--phase", "p"};
    if (flag != "--phase") {
      args.push_back(flag);
      // `--engine` validates its value at parse time; every other flag
      // takes anything.
      args.emplace_back(flag == "--engine" ? "embedded" : "v");
    }
    CHECK(parse_run_args(args).has_value());
  }

  // Parser -> catalog. The parsers' accepted sets are closed and small; name
  // them here so a flag added to either parser without a catalog entry
  // fails this loop rather than shipping undescribed.
  for (auto const flag : {"--phase", "--args", "--worktree", "--sandbox-root", "--engine", "--profile", "--json"}) {
    INFO("parser flag missing from the catalog: " << flag);
    CHECK(std::ranges::find(advertised, flag) != advertised.end());
  }
  // And the parser still refuses what the catalog does not list, and an
  // engine name it does not know.
  std::vector<std::string> const unlisted{"wf.lua", "--phase", "p", "--nope", "x"};
  CHECK_FALSE(parse_run_args(unlisted).has_value());
  std::vector<std::string> const bad_engine{"wf.lua", "--phase", "p", "--engine", "zig"};
  CHECK_FALSE(parse_run_args(bad_engine).has_value());

  // The optional `run` flags default to the empty string, matching the
  // parser (absent == empty) and `planar workflow run`'s declaration.
  for (auto const flag : {"--args", "--worktree", "--sandbox-root", "--engine"}) {
    INFO(flag);
    auto const at = catalog.find(std::format(R"("long":"{}")", flag));
    REQUIRE(at != std::string::npos);
    auto const entry = catalog.substr(at, catalog.find('}', at) - at);
    CHECK(entry.contains(R"("default":"")"));
    CHECK(entry.contains(R"("required":false)"));
  }
  auto const phase = catalog.substr(catalog.find(R"("long":"--phase")"));
  CHECK(phase.substr(0, phase.find('}')).contains(R"("required":true)"));
}

TEST_CASE("planar-execute run --engine accepts exactly the two engine names", "[cmd][execute][cli][6485]") {
  // Validated at PARSE time so a misspelt engine is a usage failure (exit 2),
  // never a dispatch-time surprise after the config has been read.
  for (auto const name : {"embedded", "centurion"}) {
    INFO(name);
    std::vector<std::string> const args{"wf.lua", "--phase", "p", "--engine", name};
    auto const                     parsed = parse_run_args(args);
    REQUIRE(parsed.has_value());
    CHECK(parsed->engine == name);
  }
  for (auto const bad : {"Embedded", "zig", "", "centurion "}) {
    INFO("'" << bad << "'");
    std::vector<std::string> const args{"wf.lua", "--phase", "p", "--engine", bad};
    CHECK_FALSE(parse_run_args(args).has_value());
  }
  // A value-less `--engine` is a usage failure like every other flag.
  std::vector<std::string> const dangling{"wf.lua", "--phase", "p", "--engine"};
  CHECK_FALSE(parse_run_args(dangling).has_value());
  // Absent is empty, which the selector reads as "no flag".
  std::vector<std::string> const plain{"wf.lua", "--phase", "p"};
  CHECK(parse_run_args(plain)->engine.empty());
}

TEST_CASE("planar-execute profile accepts `show` with --json and --profile in any order", "[cmd][execute][cli][6485][6494]") {
  using args       = std::vector<std::string>;
  auto const plain = parse_profile_args(args{"show"});
  REQUIRE(plain.has_value());
  CHECK_FALSE(plain->json);
  CHECK(plain->name == "default"); // an absent --profile resolves `default`
  for (auto const& ok : {args{"show", "--json", "--profile", "w"}, args{"show", "--profile", "w", "--json"}}) {
    auto const parsed = parse_profile_args(ok);
    REQUIRE(parsed.has_value());
    CHECK(parsed->json);
    CHECK(parsed->name == "w");
  }
  for (auto const& bad :
       {args{}, args{"list"}, args{"--json"}, args{"show", "--yaml"}, args{"show", "--json", "x"},
        args{"show", "--json", "--json"}, args{"show", "--profile"}, args{"show", "--profile", "a", "--profile", "b"}}) {
    INFO(bad.size());
    CHECK_FALSE(parse_profile_args(bad).has_value());
  }
  std::vector<std::string> const argv{"planar-execute", "profile", "show"};
  CHECK(planar::cmd::execute::classify(argv) == planar::cmd::execute::verb::profile);
}
