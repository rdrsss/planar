/// @file command.cppm
/// @brief CLI declarations for planar-agent peek.
module;
export module planar.cmd.planar_agent.handlers.peek.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::peek_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- peek ---------------------------------------------------------------
  CLI::App* peek = root.add_subcommand("peek", "Read-only what's-next selector (same query as pull, no writes).");
  shared::add_json(*peek);
  peek->add_option("plan-id")->description("Plan id to peek into")->required()->check(cliapp::zig_int_validator());
}
} // namespace planar::cmd::agent::handlers::peek_cli
