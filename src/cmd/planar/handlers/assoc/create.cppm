/// @file create.cppm
/// @brief CLI declaration for `assoc create`.
export module planar.cmd.planar.handlers.assoc.create;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
export auto attach_create(CLI::App* assoc) -> CLI::App* {
  CLI::App* create = assoc->add_subcommand("create", "Create a new association.");
  add_string(*create, "--name");
  add_string(*create, "--kind");
  add_json(*create);
  add_positional(*create, "slug");
  return create;
}
} // namespace planar::cmd::handlers::assoc_cli
