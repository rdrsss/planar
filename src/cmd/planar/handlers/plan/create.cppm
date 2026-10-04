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
  add_string(*create, "--summary", k_undocumented);
  add_string(*create, "--slug", k_undocumented);
  add_string(*create, "--scope", k_undocumented);
  add_string_default(*create, "--status", "draft", k_undocumented);
  add_int(*create, "--parent", k_undocumented);
  add_json(*create, k_undocumented);
  add_positional(*create, "title", k_undocumented);
  return create;
}
} // namespace planar::cmd::handlers::plan_cli
