/// @file list.cppm
/// @brief CLI declaration for `annotate list`.
export module planar.cmd.planar.handlers.annotate.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_list(CLI::App& annotate) -> CLI::App* {
  CLI::App* list = annotate.add_subcommand("list", "List annotations.");
  add_string(*list, "--anchor-path");
  add_string(*list, "--anchor-kind");
  add_string(*list, "--target-kind");
  add_int(*list, "--target-id");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_int(*list, "--task");
  add_string(*list, "--vendor");
  add_string(*list, "--tag");
  add_string(*list, "--scope");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::annotate_cli
