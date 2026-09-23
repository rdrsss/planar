/// @file main.cpp
/// @brief The `planar-execute` binary's entry point (plan 996, task 6107).
///
/// Port target: `main` in zig/src/cmd/planar-execute/main.zig.
///
/// Assembles the manual argument parser and delegates each verb to its
/// command-family handler. This binary has no database context or SQLite
/// handle; the handlers resolve workflow engines and Centurion profiles.
///
/// This entry point maps help and unknown verbs to their pinned exit codes;
/// each command handler maps its own outcome (see
/// `planar.cmd.planar_execute.cli` for the argument surface).
///
/// EVERY diagnostic — usage included, on the success path included — goes
/// to STDERR. stdout is the clean JSON result channel and stays empty
/// unless a workflow produced a payload. That is the opposite of the other
/// three binaries, where `--help` writes to stdout, and it was captured
/// rather than assumed.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.run.command;
import planar.cmd.planar_execute.handlers.profile.command;
import planar.cmd.planar_execute.handlers.schema.command;
import planar.cmd.planar_execute.handlers.submit.command;
import planar.cmd.planar_execute.handlers.status.command;
import planar.cmd.planar_execute.handlers.cancel.command;
import planar.cmd.planar_execute.handlers.follow.command;
import planar.cmd.planar_execute.handlers.host.command;

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

  case planar::cmd::execute::verb::schema:
    code = planar::cmd::execute::handlers::schema::execute(std::cout);
    break;

  case planar::cmd::execute::verb::profile:
    code = planar::cmd::execute::handlers::profile::execute(std::span{args}.subspan(2), std::cout, std::cerr);
    break;

  case planar::cmd::execute::verb::submit:
    code = planar::cmd::execute::handlers::submit::execute(std::span{args}.subspan(2));
    break;

  case planar::cmd::execute::verb::status:
    code = planar::cmd::execute::handlers::status::execute(std::span{args}.subspan(2));
    break;

  case planar::cmd::execute::verb::cancel:
    code = planar::cmd::execute::handlers::cancel::execute(std::span{args}.subspan(2));
    break;

  case planar::cmd::execute::verb::host:
    code = planar::cmd::execute::handlers::host::execute(std::span{args}.subspan(2));
    break;

  case planar::cmd::execute::verb::follow:
    code = planar::cmd::execute::handlers::follow::execute(std::span{args}.subspan(2));
    break;

  case planar::cmd::execute::verb::run:
    code = planar::cmd::execute::handlers::run::execute(std::span{args}.subspan(2), std::cout, std::cerr);
    break;
  }

  std::cout.flush();
  std::cerr.flush();
  return code;
}
