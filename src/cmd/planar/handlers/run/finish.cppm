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
  add_string_required(*finish, "--status", k_undocumented);
  add_json(*finish, k_undocumented);
  add_positional(*finish, "run-uid", k_undocumented);
  return finish;
}
} // namespace planar::cmd::handlers::run_cli
