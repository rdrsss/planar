/// @file touches_infer.cppm
/// @brief CLI declaration for `task touches infer`.
export module planar.cmd.planar.handlers.task.touches_infer;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the touches infer CLI node.
/// @param touches Input touches.
/// @return Registered CLI node.
export auto attach_touches_infer(CLI::App& touches) -> CLI::App* {
  CLI::App* infer = touches.add_subcommand(
      "infer",
      "Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, "
      "proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT \xe2\x80\x94 without --apply nothing is written.\n\n  Each "
      "candidate is classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching "
      "path), 'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n\n  Only 'resolved' is "
      "written by default. The wide classifications \xe2\x80\x94\n  directory and basename \xe2\x80\x94 are shown with their "
      "expansion size and\n  withheld unless --wide is passed. Measured over 46 tasks in six real\n  plans, including them "
      "yielded FEWER parallel-eligible tasks (13) than\n  resolved-only (14): a wide set intersects peers, and rule 2 drops "
      "both\n  sides of an overlap, so one loose directory mention can remove tasks\n  that were otherwise eligible.\n\n  "
      "Proposal still resolves ambiguity wide (decision 906) \xe2\x80\x94 a directory\n  expands, a basename yields every match, "
      "nothing unplaceable is\n  invented. What --wide controls is which proposals are WRITTEN.\n\n  --repo <slug> names the "
      "checkout to resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).");
  add_string(*infer, "--repo");
  add_bool(*infer, "--apply");
  add_bool(*infer, "--wide");
  add_json(*infer);
  add_positional(*infer, "task-id");
  return infer;
}
} // namespace planar::cmd::handlers::task_cli
