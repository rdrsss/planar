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
//   3. `[lint-parity]` THE VERDICT. Runs the actual, UNMODIFIED
//                      `zig/tools/cli_usage_lint` (compiled standalone at
//                      test time via `zig build-exe`, never through
//                      zig/build.zig) against `planar_cliapp_schema_stub`'s
//                      live `schema` output — on the happy path AND the
//                      enforcement path, so a "clean" result cannot be
//                      because the tool never parsed anything.
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
// The measurement is in `cli_usage_lint.zig` itself. It declares the entire
// subset of the document it reads as three structs — `SchemaJson{commands}`,
// `CommandJson{command, subcommands, flags}`, `FlagJson{long, aliases,
// short}`. Six keys. `deprecated` and the twelve-key `doc` blob are not
// among them. And they were never real on the C++ side anyway: the deleted
// `planar.cli.schema` had no `deprecated` and no per-node `doc` field on its
// own tree type either and emitted both as hardcoded constants, which this
// emitter reproduces verbatim.
//
// VERDICT: the catalog needs no separate tree representation, and
// `cli_usage_lint` runs unmodified. The `[lint-parity]` case is the standing
// proof, and it lands in this task's diff without touching a byte under
// `zig/`.
//
// SKIPs when `zig` is not on PATH or the lint tool source is missing.

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
// [lint-parity] the UNMODIFIED zig/tools/cli_usage_lint tool, run live.
// ---------------------------------------------------------------------------

namespace {

/// @brief Distinguishes WHY `build_lint_tool` didn't hand back a usable
/// binary.
///
/// Three separate situations — no `zig` on PATH, the source file missing,
/// and a non-zero `zig build-exe` — used to collapse into one `nullopt` and
/// one identical SKIP. That folded "the reference tool isn't available in
/// this environment" (a legitimate SKIP) into "the reference tool IS
/// available and its build is BROKEN" (a real regression that must FAIL).
enum class lint_tool_build_outcome : std::uint8_t { unavailable, broken, built };

/// @brief The outcome of `build_lint_tool` plus enough evidence to act on
/// it. `log` carries `build.log` whenever a build was actually attempted.
struct lint_tool_build_result {
  lint_tool_build_outcome              outcome = lint_tool_build_outcome::unavailable;
  std::optional<std::filesystem::path> bin_path;
  std::string                          log;
};

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

/// @brief Compile `src` standalone (never through zig/build.zig — this task
/// must not touch anything under `zig/`) into a scratch binary.
///
/// `src` is a parameter rather than hardcoded specifically so a test can
/// inject a deliberately-broken source file and observe `outcome == broken`
/// without needing the real tool to be broken.
/// @param src The zig source to compile.
/// @return The outcome, the binary path when built, and the build log.
auto build_lint_tool(std::filesystem::path const& src) -> lint_tool_build_result {
  if (std::system("command -v zig > /dev/null 2>&1") != 0) {
    return {.outcome = lint_tool_build_outcome::unavailable};
  }
  if (!std::filesystem::exists(src)) {
    return {.outcome = lint_tool_build_outcome::unavailable};
  }
  auto const work_dir = std::filesystem::temp_directory_path() /
                        std::format("planar_cliapp_lint_build_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(work_dir);
  auto const        out_bin   = work_dir / "cli_usage_lint";
  auto const        cache_dir = work_dir / ".zig-cache";
  auto const        log_path  = work_dir / "build.log";
  std::string const cmd_str =
      std::format("zig build-exe '{}' -O Debug --name cli_usage_lint -femit-bin='{}' --cache-dir '{}' > '{}' 2>&1", src.string(),
                  out_bin.string(), cache_dir.string(), log_path.string());
  int const status = std::system(cmd_str.c_str());

  std::string log_text;
  {
    std::ifstream in(log_path, std::ios::binary);
    log_text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::exists(out_bin)) {
    // `zig` is on PATH and the source exists — the environment IS capable
    // of building the reference tool, so a build failure here is a real
    // regression to fail on, not an unavailable-environment SKIP.
    return {.outcome = lint_tool_build_outcome::broken, .bin_path = std::nullopt, .log = log_text};
  }
  return {.outcome = lint_tool_build_outcome::built, .bin_path = out_bin, .log = log_text};
}

/// @brief Convenience overload: builds the actual, unmodified
/// `zig/tools/cli_usage_lint.zig`.
/// @return The build result.
auto build_lint_tool() -> lint_tool_build_result {
  return build_lint_tool(std::filesystem::path{PLANAR_ZIG_CLI_USAGE_LINT_SRC});
}

} // namespace

TEST_CASE("build_lint_tool: a missing source path is 'unavailable', not 'broken'", "[cliapp][schema][lint-parity][break-probe]") {
  auto const result = build_lint_tool(std::filesystem::path{"/nonexistent/planar_cliapp_lint_does_not_exist.zig"});
  CHECK(result.outcome == lint_tool_build_outcome::unavailable);
  CHECK_FALSE(result.bin_path.has_value());
}

TEST_CASE("build_lint_tool: a zig source that fails to compile is 'broken', not 'unavailable' — must FAIL, not SKIP",
          "[cliapp][schema][lint-parity][break-probe]") {
  if (std::system("command -v zig > /dev/null 2>&1") != 0) {
    SKIP("`zig` not on PATH — cannot exercise the broken-build path (this SKIP itself is the 'unavailable' case, covered "
         "by the previous test)");
  }
  auto const scratch_dir =
      std::filesystem::temp_directory_path() /
      std::format("planar_cliapp_lint_broken_src_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(scratch_dir);
  auto const broken_src = scratch_dir / "broken.zig";
  {
    std::ofstream out(broken_src, std::ios::trunc);
    out << "this is not valid zig source at all {{{ syntax error\n";
  }

  auto const result = build_lint_tool(broken_src);
  CHECK(result.outcome == lint_tool_build_outcome::broken);
  CHECK_FALSE(result.bin_path.has_value());
  CHECK_FALSE(result.log.empty()); // build.log was read back, not just written and discarded.

  std::error_code ec;
  std::filesystem::remove_all(scratch_dir, ec);
}

TEST_CASE("lint-parity: the unmodified zig cli_usage_lint accepts and enforces the CLI11-derived catalog",
          "[cliapp][schema][lint-parity]") {
  // THE SCHEMA-CATALOG VERDICT for task 6123. See this file's header.
  const std::filesystem::path stub_bin{PLANAR_CLIAPP_SCHEMA_STUB_BIN};
  if (!std::filesystem::exists(stub_bin)) {
    SKIP(std::format("schema stub binary not built at {}", stub_bin.string()));
  }
  auto const lint_build = build_lint_tool();
  if (lint_build.outcome == lint_tool_build_outcome::unavailable) {
    SKIP("`zig` not on PATH, or zig/tools/cli_usage_lint.zig is missing — cannot build the reference lint tool");
  }
  if (lint_build.outcome == lint_tool_build_outcome::broken) {
    FAIL("zig/tools/cli_usage_lint.zig failed to build even though `zig` is on PATH and the source exists — build.log:\n"
         << lint_build.log);
  }
  auto const& lint_tool = lint_build.bin_path;

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
    auto const [out, status] = capture("", lint_tool->string(), {scratch_root.string(), stub_bin.string()});
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
    auto const [out, status] = capture("", lint_tool->string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("planar task add"));
    CHECK(out.contains("--this-flag-does-not-exist"));
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
    auto const [out, status] = capture("", lint_tool->string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  std::error_code ec;
  std::filesystem::remove_all(scratch_root, ec);
}
