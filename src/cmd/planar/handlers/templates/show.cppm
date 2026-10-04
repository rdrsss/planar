/// @file show.cppm
/// @brief CLI declaration for `templates show`.
export module planar.cmd.planar.handlers.templates.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the show CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_show(CLI::App* templates) -> CLI::App* {
  CLI::App* show = templates->add_subcommand("show", "Show a template's raw JSON.");
  add_json(*show, "Emit machine-readable JSON instead of text");
  add_positional(*show, "set", "Template set name");
  add_positional(*show, "system", "External system name, e.g. jira, github-issues");
  add_positional(*show, "kind", "Template kind, e.g. epic");
  return show;
}
} // namespace planar::cmd::handlers::templates_cli
