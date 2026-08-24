/// @file main.cpp
/// @brief The `planar-execute` binary's entry point (plan 996, task 6107).
///
/// Port target: `main` in zig/src/cmd/planar-execute/main.zig.
///
/// Thin, and thinner than the other three binaries' entry points because
/// there is genuinely less to snapshot: this binary has NO `context` type,
/// NO environment lookup, NO database path resolution and NO database. It
/// reaches Planar state only by shelling `planar`/`planar-agent` — which is
/// itself part of the deferred Lua host surface — so there is nothing here
/// to thread through.
///
/// The exit-code mapping is the one decision this file makes, and all three
/// codes are oracle-captured (see `planar.cmd.planar_execute.cli`'s header
/// for the full table): 0 for `--help`, 2 for every usage failure including
/// a bare invocation, 1 for a workflow file that cannot be read.
///
/// EVERY diagnostic — usage included, on the success path included — goes
/// to STDERR. stdout is the clean JSON result channel and stays empty
/// unless a workflow produced a payload. That is the opposite of the other
/// three binaries, where `--help` writes to stdout, and it was captured
/// rather than assumed.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.engine;

namespace {

/// @brief Write the usage text to stderr, verbatim (it is a COMPLETE
/// payload — trailing newline included — so nothing is appended).
auto print_usage() -> void {
  std::cerr << planar::cmd::execute::usage_text();
}

} // namespace

/// @brief Process entry point.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return The process exit code.
auto main(int argc, char** argv) -> int {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  auto const shape = planar::cmd::execute::classify(args);

  int code = 0;
  switch (shape) {
  case planar::cmd::execute::verb::none:
    print_usage();
    code = 2;
    break;

  case planar::cmd::execute::verb::help:
    print_usage();
    code = 0;
    break;

  case planar::cmd::execute::verb::unknown:
    // Message FIRST, then usage — the oracle's order.
    std::cerr << "planar-execute: unknown verb: " << args[1] << '\n';
    print_usage();
    code = 2;
    break;

  case planar::cmd::execute::verb::run: {
    auto const parsed = planar::cmd::execute::parse_run_args(std::span{args}.subspan(2));
    if (!parsed.has_value()) {
      print_usage();
      code = 2;
      break;
    }
    // Every engine failure is exit 1. The oracle maps every `EngineError`
    // except `BadUsage` to 1, and `BadUsage` cannot come out of this path —
    // it is produced by `parse_run_args` above, which has already succeeded.
    switch (planar::cmd::execute::run_workflow(*parsed, std::cout, std::cerr)) {
    case planar::cmd::execute::run_outcome::ok:
      code = 0;
      break;
    case planar::cmd::execute::run_outcome::load_failed:
    case planar::cmd::execute::run_outcome::engine_failed:
      code = 1;
      break;
    }
    break;
  }
  }

  std::cout.flush();
  std::cerr.flush();
  return code;
}
