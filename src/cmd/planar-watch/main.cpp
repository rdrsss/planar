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
/// The Zig entry point's ONE piece of pre-parse logic, `feed` as the
/// default verb on a verb-less invocation, is reproduced — but in
/// `planar.cmd.planar_watch.dispatch::inject_default_verb` rather than
/// here, so a Catch2 case can reach it without a subprocess (task 6136).
///
/// NOT reproduced from the Zig entry point, named rather than dropped:
///
///   - `--follow`'s SIGINT handler and poll loop, which belong to the
///     streaming verbs, none of which are ported.
///   - `cli_log.record` — no `cli_log` surface exists in this tree.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.main;

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

  // The main database's path is resolved before dispatch and a failure is
  // fatal, for every verb without exception: the queue views read `planar.db`
  // too (plan 1089), so there is no domain that may go without it. The holder
  // is lazy and opens nothing until a handler asks.
  auto db_path = planar::cmd::watch::resolve_db_path(env);
  if (!db_path) {
    planar::cmd::watch::report(db_path.error(), std::cerr);
    return planar::cmd::watch::exit_code(db_path.error());
  }
  auto main_db = *std::move(db_path);

  auto                        database = std::make_shared<planar::cmd::watch::database>(std::move(main_db), std::cerr);
  planar::cmd::watch::context ctx{std::move(args), env, planar::cmd::watch::operator_cwd(env), database, std::cout, std::cerr};
  auto const                  root  = planar::cmd::watch::root_app();
  auto const                  table = planar::cmd::watch::handlers(*root);
  int const                   code  = planar::cmd::watch::run(ctx, *root, table);
  std::cout.flush();
  std::cerr.flush();
  return code;
}
