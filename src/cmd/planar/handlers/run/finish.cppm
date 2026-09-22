/// @file finish.cppm
/// @brief CLI declaration for `planar run finish`.
export module planar.cmd.planar.handlers.run.finish;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::run_cli {
export auto attach_finish(CLI::App* run) -> CLI::App* {
  CLI::App* finish = run->add_subcommand("finish", "Set the terminal status on a run.");
  add_string_required(*finish, "--status");
  add_json(*finish);
  add_positional(*finish, "run-uid");
  return finish;
}
} // namespace planar::cmd::handlers::run_cli
