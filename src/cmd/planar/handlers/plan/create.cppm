/// @file create.cppm
/// @brief CLI declaration for `planar plan create`.
export module planar.cmd.planar.handlers.plan.create;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the create CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_create(CLI::App& plan) -> CLI::App* {
  CLI::App* create = plan.add_subcommand("create", "Create a new plan.");
  add_string(*create, "--summary");
  add_string(*create, "--slug");
  add_string(*create, "--scope");
  add_string_default(*create, "--status", "draft");
  add_int(*create, "--parent");
  add_json(*create);
  add_positional(*create, "title");
  return create;
}
} // namespace planar::cmd::handlers::plan_cli
