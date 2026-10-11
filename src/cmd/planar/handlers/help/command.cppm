/// @file command.cppm
/// @brief CLI declaration for `planar help`.
///
/// `planar help` is a leaf that prints the root help page, exactly what
/// `planar --help` prints, and exits 0. It takes no arguments; per-node help
/// stays on the `--help` flag. The handler lives in `dispatch.cpp`, beside
/// `explore`'s, because it renders the root node it is registered against.
export module planar.cmd.planar.handlers.help.command;

import std;
import cli11;

namespace planar::cmd::handlers {
/// @brief Declare the `help` leaf on the root app.
/// @param root The root app to attach it to.
export auto declare_help(CLI::App& root) -> void {
  root.add_subcommand("help", "Print the root help page and exit; the same page as `planar --help`.");
}
} // namespace planar::cmd::handlers
