/// @file registry_eligibility.cppm
/// @brief CLI declaration for `models registry eligibility`.
export module planar.cmd.planar.handlers.models.registry_eligibility;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry eligibility CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_eligibility(CLI::App& registry) -> CLI::App* {
  CLI::App* eligibility =
      registry.add_subcommand("eligibility", "Report every independent eligibility gate and named exclusion reason.");
  add_int_required(*eligibility, "--candidate");
  add_string_required(*eligibility, "--host");
  add_string_required(*eligibility, "--role");
  add_string_required(*eligibility, "--tier");
  add_string_required(*eligibility, "--now");
  add_bool(*eligibility, "--override-supported");
  add_bool(*eligibility, "--policy-permits");
  return eligibility;
}
} // namespace planar::cmd::handlers::models_cli
