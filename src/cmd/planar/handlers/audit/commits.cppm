/// @file commits.cppm
/// @brief CLI declaration for `audit commits`.
export module planar.cmd.planar.handlers.audit.commits;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
export auto attach_commits(CLI::App* audit) -> CLI::App* {
  CLI::App* commits = audit->add_subcommand("commits", "List commits attributed to sessions and claims.");
  add_int(*commits, "--session");
  add_int(*commits, "--task");
  add_json(*commits);
  add_bool(*commits, "--shas");
  return commits;
}
} // namespace planar::cmd::handlers::audit_cli
