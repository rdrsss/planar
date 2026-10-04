/// @file validate.cppm
/// @brief CLI declaration for `templates validate`.
export module planar.cmd.planar.handlers.templates.validate;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the validate CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_validate(CLI::App* templates) -> CLI::App* {
  CLI::App* validate = templates->add_subcommand("validate", "Validate template syntax.");
  add_json(*validate, k_undocumented);
  add_positional(*validate, "set", k_undocumented);
  add_positional(*validate, "system", k_undocumented);
  add_positional(*validate, "kind", k_undocumented);
  return validate;
}
} // namespace planar::cmd::handlers::templates_cli
