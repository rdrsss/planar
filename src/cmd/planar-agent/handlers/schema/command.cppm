/// @file command.cppm
/// @brief CLI declarations for planar-agent schema.
module;
export module planar.cmd.planar_agent.handlers.schema.command;
import std;
import cli11;
import planar.cliapp.schema;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::schema_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* schema =
      root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
  cliapp::declare_schema_flags(*schema);
}
} // namespace planar::cmd::agent::handlers::schema_cli
