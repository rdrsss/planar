/// @file list.cppm
/// @brief CLI declaration for `annotate list`.
export module planar.cmd.planar.handlers.annotate.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the list CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_list(CLI::App& annotate) -> CLI::App* {
  CLI::App* list = annotate.add_subcommand("list", "List annotations.");
  add_string(*list, "--anchor-path", k_undocumented);
  add_string(*list, "--anchor-kind", k_undocumented);
  add_string(*list, "--target-kind", k_undocumented);
  add_int(*list, "--target-id", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_int(*list, "--plan", k_undocumented);
  add_int(*list, "--task", k_undocumented);
  add_string(*list, "--vendor", k_undocumented);
  add_string(*list, "--tag", k_undocumented);
  add_string(*list, "--scope", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::annotate_cli
