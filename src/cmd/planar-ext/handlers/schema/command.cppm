/// @file command.cppm
/// @brief CLI declaration for planar-ext schema.
module;
export module planar.cmd.planar_ext.handlers.schema.command;
import cli11;
namespace planar::cmd::ext::handlers::schema_cli {
export auto add(CLI::App& root) -> void {
  root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
}
} // namespace planar::cmd::ext::handlers::schema_cli
