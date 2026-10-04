// @file schema.t.cpp
// @brief Tests for `planar.cliapp.schema` — the `<bin> schema` JSON catalog
// emitter, rebuilt over `CLI::App` (plan 996, task 6123).
//
// Three layers, and the third is the one that decides the task's open
// question:
//
//   1. `[unit]`        structural facts about the emitted document,
//                      asserted against a small hand-built tree.
//   2. `[break-probe]` mutations of that tree that MUST change the
//                      document — a catalog emitter is trivially easy to
//                      test vacuously (a `contains("--json")` check passes
//                      against almost anything), so these prove the
//                      assertions above discriminate.
//   3. `[lint-parity]` THE VERDICT. Runs `cli_usage_lint`
//                      (`src/tools/cli_usage_lint/`, the C++ port of the
//                      former `zig/tools/cli_usage_lint.zig`, plan 996 task
//                      6402) against `planar_cliapp_schema_stub`'s live
//                      `schema` output — on the happy path AND the
//                      enforcement path, so a "clean" result cannot be
//                      because the tool never parsed anything. Before task
//                      6402 this ran the zig original, compiled standalone
//                      at test time; a differential run over this repo
//                      proved the port byte-identical to that oracle before
//                      it was retired, so re-pointing this case at the port
//                      costs the coverage nothing.
//
// ## What layer 3 settles
//
// Decision 948 recorded, as the standing risk of adopting CLI11, that "CLI11
// must expose enough structure to rebuild the schema catalog (every command,
// subcommand, flag, with the `deprecated` and `doc` fields task 6065
// requires). If it does not, the catalog emitter needs to keep its own tree
// representation." The task 6123 brief carried it forward as a verdict to
// report with evidence, since `make cli-usage-check` is a LIVE gate.
//
// The measurement is in `cli_usage_lint` itself (originally
// `cli_usage_lint.zig`; ported byte-for-byte to `src/tools/cli_usage_lint/`
// at task 6402). It declares the entire subset of the document it reads as
// three structs — `SchemaJson{commands}`, `CommandJson{command,
// subcommands, flags}`, `FlagJson{long, aliases, short}`. Six keys.
// `deprecated` and the twelve-key `doc` blob are not among them. And they
// were never real on the C++ side anyway: the deleted `planar.cli.schema`
// had no `deprecated` and no per-node `doc` field on its own tree type
// either and emitted both as hardcoded constants, which this emitter
// reproduces verbatim.
//
// VERDICT: the catalog needs no separate tree representation, and
// `cli_usage_lint` reads it unmodified. The `[lint-parity]` case is the
// standing proof.
//
// FAILs (not SKIPs) if either binary is missing at test time — both are
// unconditional add_dependencies of planar_cliapp_tests, so absence means
// this test binary's own build is stale or broken, not an environment gap.

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;

namespace {

// ---------------------------------------------------------------------------
// Fixture.
// ---------------------------------------------------------------------------

/// @brief The same `task`/`task add`/`task done` subset `schema_stub.cpp`
/// builds, duplicated here because each `*.t.cpp` compiles as its own
/// translation unit with no shared test header in this module.
/// @param app The root app to populate.
auto build_root(CLI::App& app) -> void {
  app.require_subcommand(0);

  CLI::App* task = app.add_subcommand("task", "Manage tasks.");
  task->require_subcommand(0);

  CLI::App* add = task->add_subcommand("add", "Create a new task.");
  add->add_option("--plan")->check(planar::cliapp::zig_int_validator());
  add->add_option("--slug");
  add->add_option("--priority")->check(planar::cliapp::zig_int_validator())->default_str("100");
  add->add_flag("--json");
  add->add_option("title")->required();

  CLI::App* done = task->add_subcommand("done", "Mark a task as done.");
  done->add_flag("--force")->description("Override active-claim guard and flip status anyway.");
  done->add_flag("--json");
  done->add_option("task-id")->required();
}

/// @brief Emit the fixture catalog.
/// @return The catalog JSON.
auto fixture_catalog() -> std::string {
  CLI::App app{"", "planar"};
  build_root(app);
  return planar::cliapp::schema_json(app);
}

/// @brief A tree whose PARENT carries flags and whose child REDECLARES
/// one of them — the shape `planar handoff` introduced (plan 996, task
/// 6040) and the only shape that exercises the inherited/local overlap.
/// @param app The root app to populate.
auto build_overlap_root(CLI::App& app) -> void {
  app.require_subcommand(0);

  CLI::App* handoff = app.add_subcommand("handoff", "Capture a handoff.");
  handoff->require_subcommand(0);
  handoff->add_option("--vendor");
  handoff->add_flag("--json");

  // `show` redeclares BOTH parent flags — which is how the binary makes
  // CLI11 accept `handoff show 1 --json`, since CLI11 does not inherit at
  // parse time — and adds none of its own.
  CLI::App* show = handoff->add_subcommand("show", "Show a handoff.");
  show->add_option("--vendor");
  show->add_flag("--json");
  show->add_option("handoff-id")->required();

  // `list` redeclares one and adds a genuinely local one.
  CLI::App* list = handoff->add_subcommand("list", "List handoffs.");
  list->add_flag("--json");
  list->add_option("--status");
}

/// @brief Emit the overlap fixture's catalog.
/// @return The catalog JSON.
auto overlap_catalog() -> std::string {
  CLI::App app{"", "planar"};
  build_overlap_root(app);
  return planar::cliapp::schema_json(app);
}

/// @brief Count non-overlapping occurrences of `needle` in `haystack`.
/// @param haystack The text to scan.
/// @param needle The substring to count.
/// @return The number of occurrences.
auto count_occurrences(std::string_view haystack, std::string_view needle) -> std::size_t {
  std::size_t total = 0;
  for (std::size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size())) {
    ++total;
  }
  return total;
}

/// @brief Extract one command's entry from a catalog, by its `"command"`
/// value, up to the next entry boundary.
/// @param catalog The catalog JSON.
/// @param command The full command path, e.g. `"planar handoff show"`.
/// @return That entry's text.
auto entry_for(std::string_view catalog, std::string_view command) -> std::string {
  auto const key   = std::format("\"command\":\"{}\"", command);
  auto const start = catalog.find(key);
  REQUIRE(start != std::string_view::npos);
  auto const end = catalog.find("\"name\":", start);
  return std::string{catalog.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)};
}

} // namespace

// ---------------------------------------------------------------------------
// [unit] structure.
// ---------------------------------------------------------------------------

TEST_CASE("a flag a child redeclares from its parent is emitted ONCE", "[cliapp][schema][unit]") {
  // Regression guard for plan 996 task 6040. etcli inherits a parent's
  // flags into every child at parse time; CLI11 does not, so a child that
  // must accept a parent flag REDECLARES it. That put the flag in both
  // `inherited_flags` and `local_flags`, and the catalog listed it twice —
  // which `src/cmd/catalog_parity.hpp` reported as a declaration mismatch
  // against the oracle. No group in this tree had a flag-carrying PARENT
  // before `handoff`, so nothing had exercised the overlap.
  auto const entry = entry_for(overlap_catalog(), "planar handoff show");
  CHECK(count_occurrences(entry, "\"long\":\"--json\"") == 1);
  CHECK(count_occurrences(entry, "\"long\":\"--vendor\"") == 1);
}

TEST_CASE("a redeclared flag keeps the INHERITED source label", "[cliapp][schema][unit]") {
  // The redeclaration exists only to reproduce inheritance CLI11 lacks, so
  // the surviving entry must describe the SURFACE (inherited) and not the
  // workaround (local) — which is what the oracle emits for it.
  auto const entry = entry_for(overlap_catalog(), "planar handoff show");
  auto const at    = entry.find("\"long\":\"--json\"");
  REQUIRE(at != std::string::npos);
  auto const source_at = entry.find("\"source\":", at);
  REQUIRE(source_at != std::string::npos);
  CHECK(entry.compare(source_at, std::string_view{"\"source\":\"inherited\""}.size(), "\"source\":\"inherited\"") == 0);
}

TEST_CASE("dedupe does not drop a genuinely LOCAL flag", "[cliapp][schema][unit]") {
  // The other direction: the fix must not have become "emit inherited
  // only". `--status` exists nowhere but on `list`.
  auto const entry = entry_for(overlap_catalog(), "planar handoff list");
  CHECK(count_occurrences(entry, "\"long\":\"--status\"") == 1);
  CHECK(entry.contains("\"source\":\"local\""));
  CHECK(count_occurrences(entry, "\"long\":\"--json\"") == 1);
  CHECK(count_occurrences(entry, "\"long\":\"--vendor\"") == 1);
}

TEST_CASE("dedupe is per-command, not global across the catalog", "[cliapp][schema][unit]") {
  // `--json` must still appear on the parent AND on each child; the dedupe
  // is scoped to one command's flag array. A `seen` set hoisted out of
  // `render_flags` would emit it once for the whole document.
  auto const catalog = overlap_catalog();
  CHECK(entry_for(catalog, "planar handoff").contains("\"long\":\"--json\""));
  CHECK(entry_for(catalog, "planar handoff show").contains("\"long\":\"--json\""));
  CHECK(entry_for(catalog, "planar handoff list").contains("\"long\":\"--json\""));
}

TEST_CASE("schema_json emits the envelope keys the lint tool reads", "[cliapp][schema][unit]") {
  auto const catalog = fixture_catalog();
  CHECK(catalog.starts_with(R"({"schemaVersion":1,"layout":"flat","root":"planar","commands":[)"));
  CHECK(catalog.ends_with("]}"));
  // Single-line, zero-whitespace — the shape `cli_usage_lint`'s `loadSchema`
  // parses and the shape the deleted emitter produced.
  CHECK(catalog.find('\n') == std::string::npos);
}

TEST_CASE("schema_json walks the whole tree, root included", "[cliapp][schema][unit]") {
  auto const catalog = fixture_catalog();
  CHECK(catalog.contains(R"("command":"planar")"));
  CHECK(catalog.contains(R"("command":"planar task")"));
  CHECK(catalog.contains(R"("command":"planar task add")"));
  CHECK(catalog.contains(R"("command":"planar task done")"));
  // The parent lists its children; a leaf lists none.
  CHECK(catalog.contains(R"("subcommands":["add","done"])"));
}

TEST_CASE("schema_json reports a flag's required-ness and its positionals", "[cliapp][schema][unit]") {
  auto const catalog = fixture_catalog();
  // `title` is required; `--plan` is not.
  CHECK(catalog.contains(R"("name":"title","kind":"string","required":true)"));
  CHECK(catalog.contains(R"("long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int")"));
}

TEST_CASE("schema_json carries a declared default and derives the int kind", "[cliapp][schema][unit]") {
  auto const catalog = fixture_catalog();
  // `--priority`'s default is help-only metadata on CLI11's side, but it is
  // load-bearing at the handler too (`cliapp::harvest` materializes it), so
  // the catalog must report it.
  CHECK(catalog.contains(R"("long":"--priority")"));
  // A NUMBER, not a quoted string — the oracle types the literal by kind
  // and this emitter now does too (task 6065; see `default_literal`).
  CHECK(catalog.contains(R"("default":100)"));
  CHECK_FALSE(catalog.contains(R"("default":"100")"));
  // `--json` is a flag: kind bool, no value name, and its default is FALSE
  // rather than null. CLI11 has no default STRING for a flag, but a bool
  // flag's default is not "absent" — it is `false`, which is what the
  // oracle reports for all 341 of them.
  CHECK(catalog.contains(R"("long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool")"));
  CHECK(catalog.contains(R"("kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local",)"
                         R"("valueName":"","default":false)"));
  // A value flag with NO declared default still reports null, so the two
  // cases above cannot both be "emit something non-null unconditionally".
  CHECK(catalog.contains(R"("long":"--slug")"));
  CHECK(catalog.contains(R"("valueName":"VALUE","default":null)"));
}

TEST_CASE("schema_json emits the constant deprecated/docs/completion shapes", "[cliapp][schema][unit]") {
  // Reproduced verbatim from the deleted `planar.cli.schema`, which sourced
  // them from hardcoded constants too — this emitter carries exactly as
  // much information as its predecessor, no less. See the module header.
  auto const catalog = fixture_catalog();
  CHECK(catalog.contains(R"("deprecated":null)"));
  CHECK(catalog.contains(R"("completion":{"kind":"none","values":[]})"));
  CHECK(catalog.contains(R"("docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],)"
                         R"("bugs":[],"authors":[],"homepage":"","license":"","copyright":"",)"
                         R"("version":"","sourceUrl":""}})"));
}

TEST_CASE("schema_json never lists CLI11's auto-added --help flag", "[cliapp][schema][unit]") {
  // `cli_usage_lint` carries `--help`/`-h` in its own `global_ok_flags`
  // list precisely because no binary's catalog has ever listed them.
  // Emitting them would change every command entry in the document.
  auto const catalog = fixture_catalog();
  CHECK_FALSE(catalog.contains(R"("long":"--help")"));
}

TEST_CASE("schema_json marks an inherited flag as inherited", "[cliapp][schema][unit]") {
  CLI::App app{"", "tool"};
  app.require_subcommand(0);
  app.add_flag("--verbose")->description("Chatty output");
  CLI::App* run = app.add_subcommand("run", "Run it");
  run->add_flag("--json");

  auto const catalog = planar::cliapp::schema_json(app);
  // The root declares `--verbose` LOCALLY; `run` sees it as INHERITED, and
  // its own `--json` as local. Getting the two backwards is invisible in
  // help and visible only here.
  CHECK(catalog.contains(R"("long":"--verbose")"));
  CHECK(catalog.contains(R"("source":"inherited")"));
  CHECK(catalog.contains(R"("source":"local")"));
}

TEST_CASE("schema_json recovers a choice set from CLI11's validator", "[cliapp][schema][unit]") {
  // The one piece of declared metadata that survives only because CLI11
  // renders `IsMember` into the option's type name and this emitter parses
  // it back. If that ever stops working the catalog silently loses every
  // `--category`-shaped flag's value set, so it is pinned directly.
  CLI::App app{"", "tool"};
  app.add_option("--category")->check(CLI::IsMember{std::vector<std::string>{"alpha", "beta", "gamma"}});
  auto const catalog = planar::cliapp::schema_json(app);
  CHECK(catalog.contains(R"("kind":"choice")"));
  CHECK(catalog.contains(R"("choices":["alpha","beta","gamma"])"));
}

TEST_CASE("schema_json prunes a hidden subtree entirely", "[cliapp][schema][unit]") {
  // Not merely "skips the hidden node": a VISIBLE child of a HIDDEN parent
  // is unreachable from argv, so listing it would be a lie about the
  // surface. The deleted emitter had this exact bug fixed under B3 of the
  // M2 boundary review; the walk here prunes rather than filters.
  CLI::App app{"", "tool"};
  app.require_subcommand(0);
  CLI::App* secret = app.add_subcommand("secret", "Hidden group");
  secret->group("");
  secret->require_subcommand(0);
  secret->add_subcommand("visible-child", "Looks ordinary");

  auto const catalog = planar::cliapp::schema_json(app);
  CHECK_FALSE(catalog.contains("secret"));
  CHECK_FALSE(catalog.contains("visible-child"));
}

// ---------------------------------------------------------------------------
// [break-probe] the assertions above must discriminate.
// ---------------------------------------------------------------------------

TEST_CASE("break-probe: dropping a flag changes the catalog", "[cliapp][schema][break-probe]") {
  CLI::App with{"", "planar"};
  build_root(with);

  CLI::App without{"", "planar"};
  without.require_subcommand(0);
  CLI::App* task = without.add_subcommand("task", "Manage tasks.");
  task->require_subcommand(0);
  CLI::App* add = task->add_subcommand("add", "Create a new task.");
  add->add_option("--plan")->check(planar::cliapp::zig_int_validator());
  add->add_option("--slug");
  add->add_option("--priority")->check(planar::cliapp::zig_int_validator())->default_str("100");
  // `--json` deliberately absent.
  add->add_option("title")->required();
  CLI::App* done = task->add_subcommand("done", "Mark a task as done.");
  done->add_flag("--force")->description("Override active-claim guard and flip status anyway.");
  done->add_flag("--json");
  done->add_option("task-id")->required();

  CHECK(planar::cliapp::schema_json(with) != planar::cliapp::schema_json(without));
}

TEST_CASE("break-probe: losing a positional's required-ness changes the catalog", "[cliapp][schema][break-probe]") {
  CLI::App required{"", "tool"};
  required.add_option("title")->required();
  CLI::App optional{"", "tool"};
  optional.add_option("title");
  CHECK(planar::cliapp::schema_json(required) != planar::cliapp::schema_json(optional));
  CHECK(planar::cliapp::schema_json(required).contains(R"("required":true)"));
  CHECK(planar::cliapp::schema_json(optional).contains(R"("required":false)"));
}

TEST_CASE("break-probe: losing an int validator changes the reported kind", "[cliapp][schema][break-probe]") {
  // The `zig_int_validator` is what enforces Zig's underscore-separator
  // semantics at parse time. Its absence is otherwise silent — the flag
  // still parses, it just stops refusing `10_` — so the catalog's `kind`
  // is one of the few places it is observable.
  CLI::App validated{"", "tool"};
  validated.add_option("--plan")->check(planar::cliapp::zig_int_validator());
  CLI::App bare{"", "tool"};
  bare.add_option("--plan");
  CHECK(planar::cliapp::schema_json(validated).contains(R"("kind":"int")"));
  CHECK(planar::cliapp::schema_json(bare).contains(R"("kind":"string")"));
}

// ---------------------------------------------------------------------------
// [lint-parity] `cli_usage_lint` (src/tools/, the C++ port), run live.
// ---------------------------------------------------------------------------
//
// Before task 6402 this section built `zig/tools/cli_usage_lint.zig`
// standalone at test time (`zig build-exe`) and ran THAT. The port is now a
// CMake target in this same build (`add_dependencies(planar_cliapp_tests
// cli_usage_lint)` below), so "is the reference tool available" collapses
// to "does the CMake-built binary exist at the path CMake told us about" —
// there is no separate compile-or-not outcome left to distinguish, which is
// why the old `build_lint_tool`/`lint_tool_build_outcome` machinery (three-
// way unavailable/broken/built classification, a scratch zig compile per
// call) is gone. A missing binary here means the CMake dependency edge
// itself is broken, which is a build-graph bug, not an environment gap —
// so this SKIPs rather than FAILs only to keep the test collectible when
// someone runs `ctest` against a stale build directory that predates this
// target.

namespace {

/// @brief Run `bin arg...`, redirecting stdout+stderr to a scratch file,
/// and return its contents plus the raw `std::system` exit status.
/// @param env_assignment An optional `KEY=VALUE` prefix.
/// @param bin The binary to run.
/// @param args Its arguments.
/// @return The captured output and the raw status.
auto capture(std::string const& env_assignment, std::string const& bin, std::vector<std::string> const& args)
    -> std::pair<std::string, int> {
  auto const out_path =
      std::filesystem::temp_directory_path() /
      std::format("planar_cliapp_schema_capture_{}.txt", std::chrono::steady_clock::now().time_since_epoch().count());
  std::string cmd_str;
  if (!env_assignment.empty()) {
    cmd_str += env_assignment + " ";
  }
  cmd_str += "'" + bin + "'";
  for (auto const& arg : args) {
    cmd_str += " '" + arg + "'";
  }
  cmd_str += " > " + out_path.string() + " 2>&1";
  int const status = std::system(cmd_str.c_str());

  std::ifstream   in(out_path, std::ios::binary);
  std::string     contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  return {contents, status};
}

/// @brief Count non-overlapping occurrences of `needle` in `haystack`.
///
/// Used to discriminate "reported once" from "reported once per path" when
/// a file is reachable under two names (task 6932's CLAUDE.md/AGENTS.md
/// symlink); a `contains` check cannot tell those apart.
/// @param haystack The text to search.
/// @param needle The substring to count.
/// @return The number of non-overlapping occurrences.
auto count_flag_occurrences(std::string_view haystack, std::string_view needle) -> std::size_t {
  std::size_t count  = 0;
  std::size_t cursor = 0;
  while (true) {
    auto const at = haystack.find(needle, cursor);
    if (at == std::string_view::npos)
      break;
    ++count;
    cursor = at + needle.size();
  }
  return count;
}

} // namespace

TEST_CASE("lint-parity: the ported cli_usage_lint accepts and enforces the CLI11-derived catalog",
          "[cliapp][schema][lint-parity]") {
  // THE SCHEMA-CATALOG VERDICT for task 6123, now run against the C++ port
  // (task 6402) rather than the zig original. See this section's header.
  //
  // Both binaries below are wired via an UNCONDITIONAL add_dependencies
  // edge onto planar_cliapp_tests (src/lib/cliapp/CMakeLists.txt for the
  // stub, top-level CMakeLists.txt for cli_usage_lint — the latter guarded
  // by a configure-time FATAL_ERROR if the cli_usage_lint target itself is
  // ever missing, not by an `if(TARGET ...)` that could silently skip the
  // edge). So a normal build of this test binary guarantees both exist by
  // the time this TEST_CASE runs. Absence here therefore means the build
  // that produced THIS test binary did not build its own declared
  // dependencies — a broken or stale build, not a legitimate environment
  // gap — and FAILs rather than SKIPs: a SKIP here is exactly the vacuous-
  // test hazard this file's header warns about, silently turning the
  // milestone's verdict case into a no-op that ctest still reports green.
  const std::filesystem::path stub_bin{PLANAR_CLIAPP_SCHEMA_STUB_BIN};
  if (!std::filesystem::exists(stub_bin)) {
    FAIL("schema stub binary not built at "
         << stub_bin.string()
         << " — planar_cliapp_schema_stub is an unconditional add_dependencies of planar_cliapp_tests; its absence means "
            "this test binary's own build is stale or broken, not that the reference tool is unavailable.");
  }
  const std::filesystem::path lint_tool{PLANAR_CLI_USAGE_LINT_BIN};
  if (!std::filesystem::exists(lint_tool)) {
    FAIL("cli_usage_lint binary not built at "
         << lint_tool.string()
         << " — cli_usage_lint is an unconditional add_dependencies of planar_cliapp_tests (top-level CMakeLists.txt); "
            "its absence means this test binary's own build is stale or broken, not that the reference tool is unavailable.");
  }

  // Sanity: the stub really does answer `<bin> schema` the way a real
  // Planar binary would — exit 0, JSON on stdout, which is the shape the
  // lint tool's own `loadSchema` expects.
  auto const [stub_out, stub_status] = capture("", stub_bin.string(), {"schema"});
  REQUIRE(WIFEXITED(stub_status));
  REQUIRE(WEXITSTATUS(stub_status) == 0);
  REQUIRE(stub_out.contains(R"("command":"planar task add")"));

  auto const scratch_root =
      std::filesystem::temp_directory_path() /
      std::format("planar_cliapp_lint_scratch_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(scratch_root / "agents");
  std::filesystem::create_directories(scratch_root / "skills" / "src");
  std::filesystem::create_directories(scratch_root / "docs");

  auto write_doc = [&](std::string_view text) {
    std::ofstream out(scratch_root / "docs" / "sample.md", std::ios::trunc);
    out << text;
  };

  SECTION("clean: a doc referencing a real modeled flag passes") {
    write_doc("Run `planar task add --json` to create a task.\n");
    auto const [out, status] = capture("", lint_tool.string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out.contains("clean"));
  }

  SECTION("enforcement: a doc referencing a flag the catalog does not expose is caught") {
    // The half that makes the clean result above mean something: without
    // it, a lint tool that silently failed to parse the document would
    // report "clean" too.
    write_doc("Run `planar task add --this-flag-does-not-exist` to create a task.\n");
    auto const [out, status] = capture("", lint_tool.string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("planar task add"));
    CHECK(out.contains("--this-flag-does-not-exist"));
  }

  SECTION("CLAUDE.md is scanned, and its AGENTS.md symlink is not scanned a second time") {
    // Task 6932: `k_scan_dirs` covered agents/, skills/src/ and docs/ only,
    // so CLAUDE.md -- the file every agent in this repository reads first,
    // and the densest source of claim-ritual and verb examples -- had never
    // had a single command example checked against a live catalog. Task
    // 6869 had already found a documented `planar-agent claim --metadata`
    // there that fails at parse time.
    //
    // The symlink half is not incidental: `AGENTS.md` is a symlink to
    // `CLAUDE.md` (see this repo's CLAUDE.md, "Operating Rules"), so a
    // naive directory walk would report every finding twice under two
    // paths. Asserting the COUNT is what discriminates "scanned once" from
    // "scanned twice"; asserting only `.contains("CLAUDE.md")` would pass
    // either way.
    {
      std::ofstream out(scratch_root / "CLAUDE.md", std::ios::trunc);
      out << "Run `planar task add --claude-md-only-flag` per the guide.\n";
    }
    std::error_code link_ec;
    std::filesystem::remove(scratch_root / "AGENTS.md", link_ec);
    std::filesystem::create_symlink("CLAUDE.md", scratch_root / "AGENTS.md", link_ec);
    INFO("symlink: " << link_ec.message());
    write_doc("Run `planar task add --json` to create a task.\n");

    auto const [out, status] = capture("", lint_tool.string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("--claude-md-only-flag"));
    CHECK(count_flag_occurrences(out, "--claude-md-only-flag") == 1);
    CHECK_FALSE(out.contains("AGENTS.md"));

    std::filesystem::remove(scratch_root / "CLAUDE.md", link_ec);
    std::filesystem::remove(scratch_root / "AGENTS.md", link_ec);
  }

  SECTION("scope: an unknown COMMAND path is NOT a violation, by the tool's own design") {
    // Measured, not assumed — the first draft of this case asserted the
    // opposite and failed. `cli_usage_lint` reports "any `--flag`
    // referenced on a command that the binary does not actually expose"
    // (its own header); a command path it cannot resolve at all is skipped
    // rather than flagged, because the authored surfaces legitimately
    // reference verbs that a PARTIAL catalog does not carry — which is
    // exactly the situation every C++ binary in this tree is in today
    // (fourteen of eighteen agent verbs, seven of forty-seven planar
    // verbs).
    //
    // Recorded here because it bounds what `make cli-usage-check` actually
    // guarantees against the C++ tree: flag-level drift on a PORTED verb is
    // caught; a reference to an UNPORTED verb is not. That is the same
    // guarantee the deleted emitter provided — the tool did not change.
    write_doc("Run `planar task nosuchsub --json` sometime.\n");
    auto const [out, status] = capture("", lint_tool.string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  std::error_code ec;
  std::filesystem::remove_all(scratch_root, ec);
}

TEST_CASE("select_schema narrows a flat catalog without re-rendering it", "[cliapp][schema][7204]") {
  CLI::App app{"", "tool"};
  app.require_subcommand(0);
  CLI::App* grp = app.add_subcommand("grp", "A group");
  grp->require_subcommand(0);
  grp->add_subcommand("leaf", "A leaf with \"quotes\" and a } brace");
  auto const catalog = planar::cliapp::schema_json(app);

  using planar::cliapp::schema_request;
  using planar::cliapp::select_schema;
  CHECK(select_schema(catalog, {}).value() == catalog);

  auto const one = select_schema(catalog, schema_request{.command = "grp leaf"});
  REQUIRE(one.has_value());
  CHECK(one->starts_with(R"({"name":"leaf")"));
  CHECK(catalog.contains(*one));
  CHECK(select_schema(catalog, schema_request{.command = "tool  grp   leaf"}).value() == *one);

  auto const row = select_schema(catalog, schema_request{.command = "grp leaf", .compact = true});
  CHECK(row.value() == R"({"command":"tool grp leaf","summary":"A leaf with \"quotes\" and a } brace"})");

  auto const compact = select_schema(catalog, schema_request{.compact = true});
  CHECK(compact.value() == R"({"schemaVersion":1,"layout":"compact","root":"tool","commands":[{"command":"tool","summary":""},)"
                           R"({"command":"tool grp","summary":"A group"},)"
                           R"({"command":"tool grp leaf","summary":"A leaf with \"quotes\" and a } brace"}]})");

  auto const missing = select_schema(catalog, schema_request{.command = "grp nope"});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().contains("grp nope"));
  CHECK_FALSE(select_schema(catalog, schema_request{.command = ""}).has_value());
}
