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
  add_string(*trail, "--kind", "Entity kind of the positional id (default: task)");
  add_string(*trail, "--grep", "Search pattern; switches to a query that does not widen through entity links");
  add_string(*trail, "--link", "External link id; switches to link-scoped (external_links + sync_events) form");
  add_json(*trail, "Emit machine-readable JSON instead of text");
  add_positional_optional(*trail, "entity-id", "Entity id to show the audit history of");
  return trail;
}
} // namespace planar::cmd::handlers::audit_cli
