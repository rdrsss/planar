/// @file trail.cppm
/// @brief CLI declaration for `audit trail`.
export module planar.cmd.planar.handlers.audit.trail;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
/// @brief Register the trail CLI node.
/// @param audit Input audit.
/// @return Registered CLI node.
export auto attach_trail(CLI::App* audit) -> CLI::App* {
  CLI::App* trail = audit->add_subcommand(
      "trail", "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).");
  add_string(*trail, "--kind", k_undocumented);
  add_string(*trail, "--grep", k_undocumented);
  add_string(*trail, "--link", "External link id; switches to link-scoped (external_links + sync_events) form");
  add_json(*trail, k_undocumented);
  add_positional_optional(*trail, "entity-id", k_undocumented);
  return trail;
}
} // namespace planar::cmd::handlers::audit_cli
