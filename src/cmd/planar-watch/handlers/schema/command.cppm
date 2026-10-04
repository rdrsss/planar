/// @file command.cppm
/// @brief CLI declaration for planar-watch schema.
module;
export module planar.cmd.planar_watch.handlers.schema.command;
import cli11;
import planar.cliapp.schema;
namespace planar::cmd::watch::handlers::schema_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* schema =
      root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
  cliapp::declare_schema_flags(*schema);
}
} // namespace planar::cmd::watch::handlers::schema_cli
