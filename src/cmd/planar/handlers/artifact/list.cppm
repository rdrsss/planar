/// @file list.cppm
/// @brief CLI declaration for `artifact list`.
export module planar.cmd.planar.handlers.artifact.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the list CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_list(CLI::App& artifact) -> CLI::App* {
  CLI::App* list = artifact.add_subcommand("list", "List artifacts.");
  add_string(*list, "--kind", "Restrict to a single artifact kind");
  add_string(*list, "--scope", "Scope slug to filter by (default: cwd-derived read set)");
  add_string(*list, "--status", "Filter by status: draft, active, superseded, retired");
  add_int(*list, "--plan", "Restrict to artifacts attached to this plan id");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::artifact_cli
