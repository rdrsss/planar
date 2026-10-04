/// @file run.cppm
/// @brief CLI declaration for `workflow run`.
export module planar.cmd.planar.handlers.workflow.run;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workflow_cli {
/// @brief Register the run CLI node.
/// @param workflow Input workflow.
/// @return Registered CLI node.
export auto attach_run(CLI::App* workflow) -> CLI::App* {
  CLI::App* run = workflow->add_subcommand(
      "run", "Resolve <name> across shipped and sandbox workflows, then exec\n  `planar-execute run <path> --phase <phase> "
             "[--args <json>]\n  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n  flow.result JSON streams to "
             "stdout; the exit code is forwarded\n  exactly (non-zero on flow.fail or engine error).\n\n  planar-execute "
             "resolution order: $PLANAR_EXECUTE_BIN →\n  sibling of argv[0] → PATH.");
  add_string_required(*run, "--phase", "Phase function to invoke inside the workflow.");
  add_string(*run, "--args", "JSON args blob forwarded to planar-execute --args.");
  add_string(*run, "--worktree", "Worktree directory forwarded to planar-execute --worktree.");
  add_string(*run, "--sandbox-root", "Sandbox root forwarded to planar-execute --sandbox-root.");
  add_bool(*run, "--local", "Restrict resolution to sandbox (local) workflows only.");
  add_positional(*run, "name", k_undocumented);
  return run;
}
} // namespace planar::cmd::handlers::workflow_cli
