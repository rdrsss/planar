/// @file path.cppm
/// @brief CLI declaration for `templates path`.
export module planar.cmd.planar.handlers.templates.path;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the path CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_path(CLI::App* templates) -> CLI::App* {
  CLI::App* path = templates->add_subcommand("path", "Show template resolution paths.");
  add_string(*path, "--system", k_undocumented);
  add_string(*path, "--set", k_undocumented);
  add_json(*path, k_undocumented);
  return path;
}
} // namespace planar::cmd::handlers::templates_cli
