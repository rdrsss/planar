/// @file main.cpp
/// @brief The `planar-agent` binary's entry point (plan 996, task 6107).
///
/// Deliberately thin, exactly as zig/src/cmd/planar-agent/main.zig is thin:
/// snapshot the process into a `context` and hand off. Every decision worth
/// testing lives in `planar.cmd.planar_agent.dispatch`, which takes that
/// context as a parameter and is reachable from a Catch2 case with no
/// subprocess.
///
/// One divergence from the `planar` entry point, and it is in the failure
/// path rather than the happy one: `resolve_db_path` failing here is
/// reported through `planar.cmd.planar_agent.exit`, so it carries THIS
/// binary's exit-code policy. Wiring it to the operator binary's module
/// would compile and would be wrong on two error kinds (see that module's
/// header).
///
/// NOT reproduced from the Zig entry point, each named rather than quietly
/// dropped:
///
///   - `cli_log.record`, the fail-open invocation-capture row. No
///     `cli_log` surface exists in this tree yet (same deferral the
///     `planar` entry point records).
///   - The vendor/session environment sniffing (`PLANAR_VENDOR`,
///     `CLAUDE_SESSION_ID`, ...) that the claim verbs read. It has no
///     consumer here while `pull`/`claim`/`heartbeat` are unported, and
///     adding it now would be state nothing reads.
// Not a module unit: `main` must have external linkage in the global module,
// so this translation unit only imports.
import std;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.tree;

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

  auto const env = planar::cmd::agent::process_env();

  auto db_path = planar::cmd::agent::resolve_db_path(env);
  if (!db_path) {
    planar::cmd::agent::report(db_path.error(), std::cerr);
    return planar::cmd::agent::exit_code(db_path.error());
  }

  planar::cmd::agent::context ctx{std::move(args), env, planar::cmd::agent::operator_cwd(env), *db_path, std::cout, std::cerr};
  auto const                  root  = planar::cmd::agent::root_command();
  auto const                  table = planar::cmd::agent::handlers(root);
  int const                   code  = planar::cmd::agent::run(ctx, root, table);
  std::cout.flush();
  std::cerr.flush();
  return code;
}
