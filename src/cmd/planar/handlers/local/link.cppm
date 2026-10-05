/// @file link.cppm
/// @brief CLI declaration for `local link`.
export module planar.cmd.planar.handlers.local.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the link CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_link(CLI::App* local) -> CLI::App* {
  CLI::App* link = local->add_subcommand("link", "Create or reuse symlinks from vendor paths to local source.");
  add_bool(*link, "--dry-run", "Preview the planned installs without touching the filesystem");
  add_string(*link, "--vendor", "Restrict to one vendor: claude, codex, copilot");
  add_bool(*link, "--reconcile", "Run the reconcile pass instead of linking; takes no name");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional_optional(*link, "name", "Link only the source with this name (default: all)");
  return link;
}
} // namespace planar::cmd::handlers::local_cli
