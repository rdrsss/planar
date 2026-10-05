/// @file touches_add.cppm
/// @brief CLI declaration for `task touches add`.
export module planar.cmd.planar.handlers.task.touches_add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the touches add CLI node.
/// @param touches Input touches.
/// @return Registered CLI node.
export auto attach_touches_add(CLI::App& touches) -> CLI::App* {
  CLI::App* add = touches.add_subcommand(
      "add", "Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the repo-level "
             "entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list --touches`.\n\n  With "
             "--path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the repo-level edge \xe2\x80\x94 a "
             "path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> is a repo-relative file "
             "path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  path-level declarations for rules "
             "2/3/4 (disjoint touches, migration\n  touched, singleton file touched). Declare path touches per file (repeat\n  "
             "the verb), not as a list.");
  add_string(*add, "--path", "Repo-relative file path to record as a path-level touch");
  add_string(*add, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "task-id", "Task id");
  add_positional(*add, "repo-slug", "Slug of a registered repo the task touches");
  return add;
}
} // namespace planar::cmd::handlers::task_cli
