/// @file command.cppm
/// @brief CLI declarations for the planar-watch run family.
module;
export module planar.cmd.planar_watch.handlers.run.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::run_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- run ------------------------------------------------------------
  // Folded in from `surface.cpp`'s generated `k_path_7/12/13` at task 6613
  // (M11.1). `run list`/`run show` are real handlers —
  // `dispatch.cpp` registers `handlers::run_list` / `handlers::run_show`.
  // The group node itself carries no flags and no handler; a bare
  // `planar-watch run` renders its own help page (`require_subcommand(0)`),
  // matching the prior `apply_surface`-declared shape.
  CLI::App* run = root.add_subcommand(
      "run", "Read-only view of run tables. `list` covers both workflow_runs (wf)\nand the runs table (op-arm); `show` "
             "drills into wf-source runs only.\n\n  list  \xe2\x80\x94 list runs (--plan / --status / --arm filters).\n  "
             "show  \xe2\x80\x94 drill into one wf-source run's context records.");
  run->require_subcommand(0);

  CLI::App* run_list = run->add_subcommand(
      "list", "Returns runs ordered by started_at descending.\n\n  --plan <id>    restrict to runs for the given plan.\n  "
              "--status <s>   restrict by status: running | completed | failed |\n                 interrupted | abandoned. "
              "Default: all.\n  --arm <a>      source table: wf (workflow_runs / context-plane),\n                 op (runs "
              "/ op-arm), or all (default, both).\n  --json         emit a single JSON object instead of human text.");
  shared::add_int(*run_list, "--plan", "Filter by plan id");
  run_list->add_option("--status")->description("Filter by status (default: all)");
  run_list->add_option("--arm")->description("Source arm: wf | op | all (default: all)");
  shared::add_json(*run_list);

  CLI::App* run_show = run->add_subcommand(
      "show", "Returns the full workflow_runs row for <id> plus all\n  context_records for that run, grouped and ordered "
              "by\n  stage then created_at.\n\n  Exits non-zero when the run id is unknown.");
  shared::add_json(*run_show);
  run_show->add_option("id")->description("Workflow run id (integer)")->required();
}
} // namespace planar::cmd::watch::handlers::run_cli
