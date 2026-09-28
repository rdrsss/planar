/// @file command.cppm
/// @brief CLI declaration for planar-watch completion.
module;
export module planar.cmd.planar_watch.handlers.completion.command;
import cli11;
namespace planar::cmd::watch::handlers::completion_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* completion = root.add_subcommand("completion", "Generate the autocompletion script for the specified shell.");
  completion->add_option("shell")->description("Shell: bash, zsh, or fish")->required();
}
} // namespace planar::cmd::watch::handlers::completion_cli
