/// @file command.cppm
/// @brief CLI declaration for `planar completion`.
export module planar.cmd.planar.handlers.completion.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
/// @brief Provide the declare completion command operation.
/// @param root Input root.
export auto declare_completion(CLI::App& root) -> void {
  CLI::App* completion = root.add_subcommand("completion", "Generate the autocompletion script for the specified shell.");
  add_positional(*completion, "shell", "Shell: bash, zsh, or fish");
}
} // namespace planar::cmd::handlers
