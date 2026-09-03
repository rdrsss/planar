/// @file schema_stub.cpp
/// @brief Standalone executable proving `planar.cliapp.schema::schema_json`'s
/// output is consumable by `cli_usage_lint` (plan 996, task 6123 —
/// relocated from src/lib/cli/schema_stub.cpp and re-pointed at the
/// CLI11-backed emitter; task 6402 re-pointed the consumer again, from
/// `zig/tools/cli_usage_lint.zig` to its C++ port at
/// `src/tools/cli_usage_lint/`).
///
/// `cli_usage_lint` shells `<bin_path> schema` and parses stdout as JSON
/// (see that tool's `load_schema`) — it does not care what produced the
/// binary, only that invoking it with the single argument `schema` prints
/// a conforming catalog to stdout and exits 0. This executable answers
/// that exact contract for the SAME task/task-add/task-done fixture tree
/// the deleted `src/lib/cli` tests modelled, now built as a `CLI::App`, so
/// `schema.t.cpp`'s `[lint-parity]` case can run the built `cli_usage_lint`
/// binary against it and assert on its exit code and stdout.
///
/// THAT CASE IS THE SCHEMA-CATALOG VERDICT. Decision 948 flagged "CLI11
/// must expose enough structure to rebuild the schema catalog" as the
/// standing risk of the swap; this binary plus that test case is the
/// measurement that closes it. As of task 6402 the lint tool it runs
/// against is itself a first-party CMake target under `src/tools/`, not a
/// zig source compiled at test time — the "without touching zig/" framing
/// this header used to carry no longer describes what the test does, so it
/// is retired along with the tool it referred to.
///
/// Deliberately NOT a `planar_module()` target and NOT registered under
/// `PLANAR_MODULE_TARGETS` — same pattern as `planar.core`'s
/// `vendor_probe.cpp`: pure test-proof scaffolding, never dispatches a
/// real verb, carries no engine/cmd surface.
#include <cstdio>
#include <cstring>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;

namespace {

/// @brief Same `task`/`task add`/`task done` fixture the deleted
/// src/lib/cli test suite modelled, rebuilt as a `CLI::App`. Kept as its
/// own copy since this translation unit is a standalone executable, not a
/// Catch2 test binary sharing schema.t.cpp's anonymous namespace.
/// @param app The root app to populate.
auto build_planar_root(CLI::App& app) -> void {
  app.require_subcommand(0);

  CLI::App* task = app.add_subcommand("task", "Manage tasks.");
  task->require_subcommand(0);

  CLI::App* add = task->add_subcommand("add", "Create a new task.");
  add->add_option("--body");
  add->add_option("--scope");
  add->add_option("--next-action");
  add->add_option("--due");
  add->add_option("--plan")->check(planar::cliapp::zig_int_validator());
  add->add_option("--parent")->check(planar::cliapp::zig_int_validator());
  add->add_option("--slug");
  add->add_option("--priority")->check(planar::cliapp::zig_int_validator())->default_str("100");
  add->add_flag("--editor");
  add->add_flag("--no-auto-promote");
  add->add_flag("--json");
  add->add_option("title")->required();

  CLI::App* done = task->add_subcommand("done", "Mark a task as done (single-arg form; Go supports variadic).");
  done->add_option("--scope");
  done->add_flag("--force")->description("Override active-claim guard and flip status anyway.");
  done->add_flag("--json");
  done->add_option("task-id")->required();
}

} // namespace

/// @brief Answer `<this> schema` exactly the way `cli_usage_lint`
/// expects of a real Planar binary (see this file's header comment).
/// @param argc Argument count; must be at least 2 (`argv[1] == "schema"`).
/// @param argv Argument vector; `argv[1]` must be the literal token
/// `"schema"` — any other invocation prints a usage message and exits 2.
/// @return 0 with the fixture catalog on stdout for `<this> schema`; 2 on
/// any other invocation.
auto main(int argc, char** argv) -> int {
  if (argc < 2 || std::strcmp(argv[1], "schema") != 0) {
    std::fprintf(stderr, "usage: %s schema\n", argc > 0 ? argv[0] : "schema_stub");
    return 2;
  }
  CLI::App app{"", "planar"};
  build_planar_root(app);
  auto const catalog = planar::cliapp::schema_json(app);
  std::fputs(catalog.c_str(), stdout);
  std::fputc('\n', stdout);
  return 0;
}
