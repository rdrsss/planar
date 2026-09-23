/// @file compute.cppm
/// @brief CLI declaration for `closure compute`.
export module planar.cmd.planar.handlers.closure.compute;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::closure_cli {
/// @brief Register the compute CLI node.
/// @param closure Input closure.
/// @return Registered CLI node.
export auto attach_compute(CLI::App* closure) -> CLI::App* {
  CLI::App* compute = closure->add_subcommand("compute", "Run the extractor over a task's seeds and persist the closure.");
  add_string(*compute, "--scope");
  add_json(*compute);
  add_positional(*compute, "task-id");
  return compute;
}
} // namespace planar::cmd::handlers::closure_cli
