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
  add_string(*create, "--name", "Human-readable name (default: the slug)");
  add_string(*create, "--kind", "Association kind: org, project, client, personal, ad-hoc (default: ad-hoc)");
  add_json(*create, "Emit machine-readable JSON instead of text");
  add_positional(*create, "slug", "Unique association slug, e.g. org:acme");
  return create;
}
} // namespace planar::cmd::handlers::assoc_cli
