/// @file show.cppm
/// @brief CLI declaration for `closure show`.
export module planar.cmd.planar.handlers.closure.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::closure_cli {
/// @brief Register the show CLI node.
/// @param closure Input closure.
/// @return Registered CLI node.
export auto attach_show(CLI::App* closure) -> CLI::App* {
  CLI::App* show = closure->add_subcommand("show", "Read back a task's persisted closure rows.");
  add_json(*show);
  add_positional(*show, "task-id");
  return show;
}
} // namespace planar::cmd::handlers::closure_cli
