/// @file schema_stub.cpp
/// @brief Standalone executable proving `planar.cli.schema::schema_json`'s
/// output is consumable, UNMODIFIED, by `zig/tools/cli_usage_lint` (plan
/// 996, task cpp-cli-schema-catalog).
///
/// `cli_usage_lint` shells `<bin_path> schema` and parses stdout as JSON
/// (see that file's `loadSchema`) — it does not care what produced the
/// binary, only that invoking it with the single argument `schema` prints
/// a conforming catalog to stdout and exits 0. This executable answers
/// that exact contract for the SAME task/task-add/task-done fixture tree
/// `help.t.cpp`/`parser.t.cpp` already model (see those files' header
/// comments for why this subset was chosen), so `schema.t.cpp`'s
/// `[lint-parity]` case can run the real, unmodified lint tool against it
/// and assert on its exit code and stdout — proof that lands in this
/// task's diff without touching a single byte under `zig/`.
///
/// Deliberately NOT a `planar_module()` target and NOT registered under
/// `PLANAR_MODULE_TARGETS` — same pattern as `planar.core`'s
/// `vendor_probe.cpp`: pure test-proof scaffolding, never dispatches a
/// real verb, carries no engine/cmd surface.
#include <cstdio>
#include <cstring>

import std;
import planar.cli;

namespace {

using planar::cli::cmd;
using planar::cli::flag;
using planar::cli::positional;

/// @brief Same `task`/`task add`/`task done` fixture as
/// help.t.cpp/parser.t.cpp/schema.t.cpp's `make_planar_root()` — kept as
/// its own copy since this translation unit is a standalone executable,
/// not a Catch2 test binary sharing those files' anonymous namespace.
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

} // namespace

/// @brief Answer `<this> schema` exactly the way `zig/tools/cli_usage_lint`
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
  auto const catalog = planar::cli::schema_json(make_planar_root());
  std::fputs(catalog.c_str(), stdout);
  std::fputc('\n', stdout);
  return 0;
}
