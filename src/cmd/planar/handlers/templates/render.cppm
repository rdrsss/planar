/// @file render.cppm
/// @brief CLI declaration for `templates render`.
export module planar.cmd.planar.handlers.templates.render;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the render CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_render(CLI::App* templates) -> CLI::App* {
  CLI::App* render = templates->add_subcommand("render", "Render a template against a database entity (dry run; no writes).");
  add_string_required(*render, "--entity", "Entity ref (kind:id) — task:42, plan:7, scenario:3");
  add_json(*render, k_undocumented);
  add_positional(*render, "set", k_undocumented);
  add_positional(*render, "system", k_undocumented);
  add_positional(*render, "kind", k_undocumented);
  return render;
}
} // namespace planar::cmd::handlers::templates_cli
