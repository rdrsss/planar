/// @file command.cppm
/// @brief CLI declarations for planar-agent run.
module;
export module planar.cmd.planar_agent.handlers.run.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::run_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- run start / end / heartbeat ----------------------------------------
  CLI::App* run = root.add_subcommand(
      "run", "Workflow run lifecycle (start / end / heartbeat). Used by an external workflow harness to stay DB-handle-free.");
  run->require_subcommand(0);
  CLI::App* run_start = run->add_subcommand("start", "Insert a workflow_runs row in running status.");
  run_start->add_option("--plan")->description("Plan id the run belongs to")->required();
  run_start->add_option("--workflow")->description("Workflow name (e.g. isolated-sequential)")->required();
  run_start->add_option("--run-id")->description("Unique run identifier (run-<pid>-<nanos>)")->required();
  run_start->add_option("--pid")->description(
      "PID of the external workflow harness process. Exactly one of --pid / --ttl is required: a pid-bound run is "
      "probed for liveness by reconcile; a pid-less run needs --ttl instead");
  run_start->add_option("--ttl")->description(
      "Lease TTL for a pid-less run (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms), extended by "
      "`run heartbeat` and enforced by reconcile. Required when --pid is omitted");
  run_start->add_option("--repo-root")->description("Absolute path of the repo root the harness is driving")->required();
  shared::add_json(*run_start, "Emit machine-readable JSON instead of text");

  CLI::App* run_end =
      run->add_subcommand("end", "Close a workflow_runs row with a terminal status (completed|failed|interrupted).");
  run_end->add_option("--run-id")->description("The run identifier (run-<pid>-<nanos>) returned by run start")->required();
  run_end->add_option("--status")->description("Terminal status: completed | failed | interrupted")->required();
  shared::add_json(*run_end, "Emit machine-readable JSON instead of text");

  CLI::App* run_heartbeat = run->add_subcommand("heartbeat", "Extend a pid-less run's lease (expires_at).");
  run_heartbeat->add_option("--run-id")->description("The run identifier (run-<pid>-<nanos>) returned by run start")->required();
  run_heartbeat->add_option("--ttl")
      ->description(
          "New lease TTL, set absolutely from now (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms). Refuses "
          "on a pid-supervised run or a run no longer `running`")
      ->required();
  shared::add_json(*run_heartbeat, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::run_cli
