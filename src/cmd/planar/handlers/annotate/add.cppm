/// @file add.cppm
/// @brief CLI declaration for `annotate add`.
export module planar.cmd.planar.handlers.annotate.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the add CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_add(CLI::App& annotate) -> CLI::App* {
  CLI::App* add = annotate.add_subcommand("add", "Create a new annotation.");
  add_string(*add, "--anchor-path", k_undocumented);
  add_int(*add, "--line-start", k_undocumented);
  add_int(*add, "--line-end", k_undocumented);
  add_string(*add, "--commit-sha", k_undocumented);
  add_string(*add, "--text-hash", k_undocumented);
  add_string(*add, "--text", k_undocumented);
  add_string(*add, "--title", k_undocumented);
  add_string(*add, "--slug", k_undocumented);
  add_string(*add, "--body", k_undocumented);
  add_string(*add, "--vendor", k_undocumented);
  add_int(*add, "--plan", k_undocumented);
  add_int(*add, "--task", k_undocumented);
  add_string(*add, "--tags", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_json(*add, k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::annotate_cli
