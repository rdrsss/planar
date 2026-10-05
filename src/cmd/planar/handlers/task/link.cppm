/// @file link.cppm
/// @brief CLI declaration for `task link`.
export module planar.cmd.planar.handlers.task.link;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the link CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_link(CLI::App& task) -> CLI::App* {
  CLI::App* link = task.add_subcommand("link", "Create an entity link from a task to another entity.");
  add_string(*link, "--relationship",
             "Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches");
  add_string(*link, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional(*link, "task-id", "Task id");
  add_positional(*link, "ref", "Target entity ref (kind:id)");
  return link;
}
} // namespace planar::cmd::handlers::task_cli
