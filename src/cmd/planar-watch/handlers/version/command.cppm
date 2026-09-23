/// @file command.cppm
/// @brief CLI declaration for planar-watch version.
module;
export module planar.cmd.planar_watch.handlers.version.command;
import cli11;
namespace planar::cmd::watch::handlers::version_cli {
export auto add(CLI::App& root) -> void {
  root.add_subcommand("version", "Print the planar-watch version, commit, and C++ toolchain.");
}
} // namespace planar::cmd::watch::handlers::version_cli
