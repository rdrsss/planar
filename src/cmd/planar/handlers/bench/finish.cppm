/// @file finish.cppm
/// @brief CLI declaration for `planar bench finish`.
export module planar.cmd.planar.handlers.bench.finish;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the finish CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_finish(CLI::App* bench) -> CLI::App* {
  CLI::App* finish = bench->add_subcommand("finish", "Set the terminal status on a run.");
  add_string_required(*finish, "--status");
  add_positional(*finish, "run-uid");
  return finish;
}
} // namespace planar::cmd::handlers::bench_cli
