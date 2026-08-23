/// @file main.cpp
/// @brief The `planar-watch` binary's entry point (plan 996, task 6107).
///
/// Thin, exactly as zig/src/cmd/planar-watch/main.zig is thin: snapshot the
/// process into a `context` and hand off to
/// `planar.cmd.planar_watch.dispatch`, which is reachable from a Catch2
/// case with no subprocess.
///
/// The context this builds is the READ-ONLY one — that is the whole
/// distinction of this binary, and it lives in
/// `planar.cmd.planar_watch.context::ensure_db`, not here. Nothing in this
/// file may open a database; a viewer that bootstrapped state on startup
/// would break the invariant its own `--help` page advertises.
///
/// NOT reproduced from the Zig entry point, named rather than dropped:
///
///   - The `feed` default verb on a bare invocation. `feed` is unported
///     (blocked on `engine.runtime.agentactivity`); a bare invocation
///     renders the root help page instead. See
///     `planar.cmd.planar_watch.dispatch`'s header.
///   - `--follow`'s SIGINT handler and poll loop, which belong to the
///     streaming verbs, none of which are ported.
///   - `cli_log.record` — no `cli_log` surface exists in this tree.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.tree;

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

  auto const env = planar::cmd::watch::process_env();

  auto db_path = planar::cmd::watch::resolve_db_path(env);
  if (!db_path) {
    planar::cmd::watch::report(db_path.error(), std::cerr);
    return planar::cmd::watch::exit_code(db_path.error());
  }

  planar::cmd::watch::context ctx{std::move(args), env, planar::cmd::watch::operator_cwd(env), *db_path, std::cout, std::cerr};
  auto const                  root  = planar::cmd::watch::root_app();
  auto const                  table = planar::cmd::watch::handlers(*root);
  int const                   code  = planar::cmd::watch::run(ctx, *root, table);
  std::cout.flush();
  std::cerr.flush();
  return code;
}
