/// @file create.cppm
/// @brief CLI declaration for `assoc create`.
export module planar.cmd.planar.handlers.assoc.create;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
/// @brief Register the create CLI node.
/// @param assoc Input assoc.
/// @return Registered CLI node.
export auto attach_create(CLI::App* assoc) -> CLI::App* {
  CLI::App* create = assoc->add_subcommand("create", "Create a new association.");
  add_string(*create, "--name", k_undocumented);
  add_string(*create, "--kind", k_undocumented);
  add_json(*create, k_undocumented);
  add_positional(*create, "slug", k_undocumented);
  return create;
}
} // namespace planar::cmd::handlers::assoc_cli
