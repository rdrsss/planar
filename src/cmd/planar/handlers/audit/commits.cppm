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
  add_int(*commits, "--session", k_undocumented);
  add_int(*commits, "--task", k_undocumented);
  add_json(*commits, k_undocumented);
  add_bool(*commits, "--shas", k_undocumented);
  return commits;
}
} // namespace planar::cmd::handlers::audit_cli
