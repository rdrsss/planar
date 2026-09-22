/// @file add.cppm
/// @brief CLI declaration for `assoc add`.
export module planar.cmd.planar.handlers.assoc.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
export auto attach_add(CLI::App* assoc) -> CLI::App* {
  CLI::App* add = assoc->add_subcommand("add", "Add a repo to an association.");
  add_json(*add);
  add_positional(*add, "slug");
  add_positional(*add, "repo-path");
  return add;
}
} // namespace planar::cmd::handlers::assoc_cli
