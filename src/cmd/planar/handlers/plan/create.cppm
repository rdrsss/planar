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
  add_string(*create, "--summary", "Plan summary text; a leading @ reads it from a file");
  add_string(*create, "--slug", "Slug for the plan (derived from the title when omitted)");
  add_string(*create, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_string_default(*create, "--status", "draft", "Initial status: draft, active, paused, done, abandoned");
  add_int(*create, "--parent", "Parent plan id");
  add_json(*create, "Emit machine-readable JSON instead of text");
  add_positional(*create, "title", "Title of the new plan");
  return create;
}
} // namespace planar::cmd::handlers::plan_cli
