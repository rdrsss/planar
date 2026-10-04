/// @file command.cppm
/// @brief CLI declarations for planar-agent heartbeat.
module;
export module planar.cmd.planar_agent.handlers.heartbeat.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::heartbeat_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- heartbeat ----------------------------------------------------------
  CLI::App* heartbeat = root.add_subcommand("heartbeat", "Refresh the lease on an active claim.");
  shared::add_claim(*heartbeat, "Claim token to refresh");
  shared::add_heartbeat_ttl(*heartbeat);
  heartbeat->add_option("--status")->description("Free-text status string recorded on the heartbeat action row's summary column");
  shared::add_supervision(*heartbeat, false);
  shared::add_json(*heartbeat, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::heartbeat_cli
