/// @file start.cppm
/// @brief CLI declaration for `planar bench start`.
export module planar.cmd.planar.handlers.bench.start;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the start CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_start(CLI::App* bench) -> CLI::App* {
  CLI::App* start = bench->add_subcommand(
      "start",
      "Mint a new run record and print its run_uid.\n\n  --task <id> (repeatable): limit the declared-touch snapshot to\n  the "
      "given task ids. When omitted, all plan tasks are snapshotted\n  (backward-compatible default). Use when the arm only "
      "dispatches\n  a known subset of tasks and meta-tasks with no touches would\n  otherwise inflate the declared set.");
  add_int_required(*start, "--plan", k_undocumented);
  add_string_required(*start, "--arm", k_undocumented);
  add_string_required(*start, "--base-sha", k_undocumented);
  add_string_required(*start, "--config-hash", k_undocumented);
  add_string(*start, "--config-json", k_undocumented);
  add_string(*start, "--corpus-repo", k_undocumented);
  add_string_list(*start, "--task", "Limit declared-touch snapshot to this task id (repeatable).");
  add_positional(*start, "run-uid", k_undocumented);
  return start;
}
} // namespace planar::cmd::handlers::bench_cli
