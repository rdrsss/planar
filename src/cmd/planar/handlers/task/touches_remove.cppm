/// @file touches_remove.cppm
/// @brief CLI declaration for `task touches remove`.
export module planar.cmd.planar.handlers.task.touches_remove;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the touches remove CLI node.
/// @param touches Input touches.
/// @return Registered CLI node.
export auto attach_touches_remove(CLI::App& touches) -> CLI::App* {
  CLI::App* remove = touches.add_subcommand(
      "remove", "Withdraw a touch declaration.\n\n  Without --path: removes the repo-level entity_links 'touches' edge.\n\n  "
                "With --path <p>: removes ONE path-level task_touch_paths row and leaves\n  the repo edge in place. Deliberately "
                "not symmetric with `touches add`,\n  where a path-touch implies the repo-touch \xe2\x80\x94 withdrawing one "
                "file should\n  not silently drop a repo claim that may carry other paths.\n\n  Removing the repo edge is not a "
                "substitute for --path: the parallel\n  eligibility rules read task_touch_paths directly, so orphaned path "
                "rows\n  keep driving eligibility after their edge is gone.");
  add_string(*remove, "--path", k_undocumented);
  add_string(*remove, "--scope", k_undocumented);
  add_json(*remove, k_undocumented);
  add_positional(*remove, "task-id", k_undocumented);
  add_positional(*remove, "repo-slug", k_undocumented);
  return remove;
}
} // namespace planar::cmd::handlers::task_cli
