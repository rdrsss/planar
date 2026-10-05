/// @file lint.cppm
/// @brief CLI declaration for `workbench lint`.
export module planar.cmd.planar.handlers.workbench.lint;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the lint CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_lint(CLI::App& workbench) -> CLI::App* {
  CLI::App* lint = workbench.add_subcommand("lint", "Validate workbench Markdown frontmatter without syncing.");
  add_bool(*lint, "--all", "Validate every workbench tree");
  add_string(*lint, "--path", "Validate one Markdown file or directory");
  add_json(*lint, "Emit machine-readable JSON instead of text");
  add_positional_optional(*lint, "plan", "Plan id or slug (or use --all / --path)");
  return lint;
}
} // namespace planar::cmd::handlers::workbench_cli
