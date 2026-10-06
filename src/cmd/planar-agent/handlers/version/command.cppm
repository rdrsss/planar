/// @file command.cppm
/// @brief CLI declarations for planar-agent version.
module;
export module planar.cmd.planar_agent.handlers.version.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::version_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  root.add_subcommand("version", "Print the planar-agent version, commit, and C++ toolchain.")
      ->add_flag("--json", "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::version_cli
