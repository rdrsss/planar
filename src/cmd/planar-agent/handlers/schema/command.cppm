/// @file command.cppm
/// @brief CLI declarations for planar-agent schema.
module;
export module planar.cmd.planar_agent.handlers.schema.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::schema_cli {
export auto add(CLI::App& root) -> void {
  root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
}
} // namespace planar::cmd::agent::handlers::schema_cli
