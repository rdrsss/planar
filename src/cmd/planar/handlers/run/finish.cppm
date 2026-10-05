/// @file finish.cppm
/// @brief CLI declaration for `planar run finish`.
export module planar.cmd.planar.handlers.run.finish;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::run_cli {
/// @brief Register the finish CLI node.
/// @param run Input run.
/// @return Registered CLI node.
export auto attach_finish(CLI::App* run) -> CLI::App* {
  CLI::App* finish = run->add_subcommand("finish", "Set the terminal status on a run.");
  add_string_required(*finish, "--status", "Terminal status: completed, aborted, error");
  add_json(*finish, "Emit machine-readable JSON instead of text");
  add_positional(*finish, "run-uid", "Run uid returned by run start");
  return finish;
}
} // namespace planar::cmd::handlers::run_cli
