/// @file command.cppm
/// @brief CLI declaration for `planar run`.
export module planar.cmd.planar.handlers.run.command;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.run.start;
import planar.cmd.planar.handlers.run.event;
import planar.cmd.planar.handlers.run.finish;
import planar.cmd.planar.handlers.run.show;

namespace planar::cmd::handlers {
/// @brief Provide the declare run command operation.
/// @param root Input root.
export auto declare_run(CLI::App& root) -> void {
  CLI::App* run = root.add_subcommand(
      "run", "Record operational run traces emitted by workflows.\n\n  Arm defaults to 'op' (or the workflow name when "
             "--workflow is given).\n  Statuses: running, completed, aborted, error.\n\n  Workflow: run start → run event "
             "(repeat) → run finish → run show --json.\n\n  See `planar bench` for the measurement-rig surface "
             "(strict/eligibility/\n  grouped arms, declared/actual touch tracking, git-diff harvest).");
  run->require_subcommand(0);

  run_cli::attach_start(run);

  run_cli::attach_event(run);

  run_cli::attach_finish(run);

  run_cli::attach_show(run);
}
} // namespace planar::cmd::handlers
