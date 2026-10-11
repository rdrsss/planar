/// @file command.cppm
/// @brief CLI declarations for planar-agent brief.
module;
export module planar.cmd.planar_agent.handlers.brief.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::brief_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* brief = root.add_subcommand(
      "brief", "Read-only single entry point: one plan's state, claims, parallel wave, readiness, policy and claim templates.");
  shared::add_json(*brief, "Emit machine-readable JSON (brief_version first, fixed key order) instead of text");
  brief->add_option("plan-id")->description("Plan id (a milestone or an anchor)")->required()->check(cliapp::zig_int_validator());
}
} // namespace planar::cmd::agent::handlers::brief_cli
