/// @file command.cppm
/// @brief CLI declaration for planar-watch schema.
module;
export module planar.cmd.planar_watch.handlers.schema.command;
import cli11;
namespace planar::cmd::watch::handlers::schema_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
}
} // namespace planar::cmd::watch::handlers::schema_cli
