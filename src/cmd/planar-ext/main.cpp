/// @file main.cpp
/// @brief The `planar-ext` binary's entry point (plan 996, task 6418).
///
/// Thin, exactly like its three siblings: snapshot the process into a
/// `context` and hand off to `planar.cmd.planar_ext.dispatch`, which is
/// reachable from a Catch2 case with no subprocess.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.tree;

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

  auto const env = planar::cmd::ext::process_env();

  auto db_path = planar::cmd::ext::resolve_db_path(env);
  if (!db_path) {
    planar::cmd::ext::report(db_path.error(), std::cerr);
    return planar::cmd::ext::exit_code(db_path.error());
  }

  planar::cmd::ext::context ctx{std::move(args), env, planar::cmd::ext::operator_cwd(env), *db_path, std::cout, std::cerr};
  auto const                root  = planar::cmd::ext::root_app();
  auto const                table = planar::cmd::ext::handlers(*root);
  int const                 code  = planar::cmd::ext::run(ctx, *root, table);
  std::cout.flush();
  std::cerr.flush();
  return code;
}
