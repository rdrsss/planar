/// @file commits.cppm
/// @brief CLI declaration for `audit commits`.
export module planar.cmd.planar.handlers.audit.commits;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
/// @brief Register the commits CLI node.
/// @param audit Input audit.
/// @return Registered CLI node.
export auto attach_commits(CLI::App* audit) -> CLI::App* {
  CLI::App* commits = audit->add_subcommand("commits", "List commits attributed to sessions and claims.");
  add_int(*commits, "--session", "Only commits recorded for this session id");
  add_int(*commits, "--task", "Only commits recorded for claims on this task id");
  add_json(*commits, "Emit machine-readable JSON instead of text");
  add_bool(*commits, "--shas", "Emit bare commit SHAs, one per line; mutually exclusive with --json");
  return commits;
}
} // namespace planar::cmd::handlers::audit_cli
