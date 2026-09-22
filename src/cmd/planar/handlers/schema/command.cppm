/// @file command.cppm
/// @brief CLI declaration for `planar schema`.
export module planar.cmd.planar.handlers.schema.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
export auto declare_schema(CLI::App& root) -> void {
  root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
}
} // namespace planar::cmd::handlers
