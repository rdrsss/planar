/// @file command.cppm
/// @brief CLI declaration for planar-ext version.
module;
export module planar.cmd.planar_ext.handlers.version.command;
import cli11;
namespace planar::cmd::ext::handlers::version_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  root.add_subcommand("version", "Print the planar-ext version, commit, and compiler.")
      ->add_flag("--json", "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::ext::handlers::version_cli
