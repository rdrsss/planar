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
  add_int_required(*start, "--plan", "Plan id whose tasks are snapshotted as declared touches");
  add_string_required(*start, "--arm", "Experiment arm: strict, eligibility, grouped, or free text");
  add_string_required(*start, "--base-sha", "Base commit sha the run starts from");
  add_string_required(*start, "--config-hash", "Hash identifying the run configuration");
  add_string(*start, "--config-json", "Run configuration; must be valid JSON");
  add_string(*start, "--corpus-repo", "Corpus repository the run operates on");
  add_string_list(*start, "--task", "Limit declared-touch snapshot to this task id (repeatable).");
  add_positional(*start, "run-uid", "Run uid to mint");
  return start;
}
} // namespace planar::cmd::handlers::bench_cli
