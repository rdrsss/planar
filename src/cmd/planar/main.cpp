/// @file main.cpp
/// @brief The `planar` operator binary's entry point (plan 996, task 6105).
///
/// Deliberately thin, exactly as zig/src/cmd/planar/main.zig is thin: it
/// snapshots the process (argv, environment, cwd, database path, the two
/// standard streams) into a `context` and hands off. Every decision worth
/// testing — parsing, routing, help, exit codes — lives in
/// `planar.cmd.planar.dispatch`, which takes that context as a parameter
/// and is therefore reachable from a Catch2 case without a subprocess.
///
/// NOT reproduced from the Zig entry point, each named rather than quietly
/// dropped:
///
///   - The bare-invocation TTY cockpit gate (`args.len == 1` +
///     `cockpit_gate.check` -> launch the interactive cockpit). There is no
///     cockpit in this tree. A bare `planar` here falls through to the
///     root help page, which is exactly what the Zig binary does when the
///     gate refuses (non-TTY, `TERM=dumb`, `PLANAR_NO_TUI`) — i.e. what
///     every scripted invocation already sees.
///   - The worktree gate (`worktree_gate.check`), deferred with its
///     git-subprocess dependency. See `planar.cmd.planar.scope`'s header.
///   - `cli_log.record`, the fail-open invocation-capture row. No
///     `cli_log` surface exists in this tree yet.
// Not a module unit: `main` must have external linkage in the global module,
// so this translation unit only imports.
import std;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.exit;
import planar.cmd.planar.tree;

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

  auto const env = planar::cmd::process_env();

  auto db_path = planar::cmd::resolve_db_path(env);
  if (!db_path) {
    planar::cmd::report(db_path.error(), std::cerr);
    return planar::cmd::exit_code(db_path.error());
  }

  planar::cmd::context ctx{std::move(args), env, planar::cmd::operator_cwd(env), *db_path, std::cout, std::cerr};
  auto const           root  = planar::cmd::root_app();
  auto const           table = planar::cmd::handlers();
  int const            code  = planar::cmd::run(ctx, *root, table);
  std::cout.flush();
  std::cerr.flush();
  return code;
}
