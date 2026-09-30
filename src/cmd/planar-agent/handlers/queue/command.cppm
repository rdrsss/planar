/// @file command.cppm
/// @brief CLI declarations for the `planar-agent queue` domain.
module;
export module planar.cmd.planar_agent.handlers.queue.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
namespace planar::cmd::agent::handlers::queue_cli {
/// @brief Register the `queue` domain and its verbs.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* queue = root.add_subcommand("queue", "Host-wide build and test queue (run a command in turn).");
  queue->require_subcommand(0);

  // --- queue run ----------------------------------------------------------
  // The foreground form and, with `--detach`, the detached form. `--claim` is
  // declared by the task that implements it (tech spec 647 § CLI surface).
  CLI::App* run = queue->add_subcommand(
      "run", "Run a command in turn, host-wide: wait for the command's turn in the queue, run it in the caller's directory "
             "with the caller's environment, and exit with its status.");
  run->add_flag("--detach")
      ->description(
          "Return a ticket at once and let a detached submitter wait, run the command and record its history: print the "
          "sequence number and the path of the output file <agent-db-directory>/queue-logs/<seq>.log, one per line, and "
          "exit 0; exit 125 when no ticket could be issued");
  run->add_option("--timeout")
      ->description(
          "How long the command may run before it is stopped (SIGTERM, then SIGKILL after the grace period): an integer "
          "and a unit, ms, s, m or h, at most 24h; default 30m");
  run->add_option("--wait-timeout")
      ->description(
          "How long the entry may wait for its turn before it is removed without running: an integer and a unit, ms, s, m "
          "or h, at most 24h; default no limit");
  run->add_option("--label")->description("Operator-facing label recorded with the queue entry");
  run->add_option("--vendor")
      ->description(
          "The submitting agent's vendor, recorded with the queue entry; default $PLANAR_VENDOR, empty when neither is set");
  run->add_option("--role")->description(
      "The submitting agent's role, recorded with the queue entry; default $PLANAR_ROLE, empty when neither is set");
  run->add_flag("--notices")
      ->description(
          "Write the entry's queue position, start and final outcome to standard error; standard output is never touched");
  run->add_option("command")->description("The command and its arguments, after `--`")->required()->expected(1, -1);

  // --- queue cancel -------------------------------------------------------
  // Any caller that can open the store may cancel any entry; the canceller is
  // recorded (tech spec 647 § CLI surface, § Cancellation is open to every
  // caller and is attributed).
  CLI::App* cancel = queue->add_subcommand(
      "cancel", "Cancel a queue entry: remove a waiting one, or stop a running one (SIGTERM to its command's process group, "
                "then SIGKILL after the grace period if the group still has members) and wait until its group is empty.");
  cancel->add_option("--vendor")
      ->description(
          "The cancelling agent's vendor, recorded as who cancelled; default $PLANAR_VENDOR, empty when neither is set");
  cancel->add_option("--role")->description(
      "The cancelling agent's role, recorded as who cancelled; default $PLANAR_ROLE, empty when neither is set");
  cancel->add_option("seq")
      ->description("The entry's sequence number, as `queue run --detach` and the notices name it")
      ->required()
      ->expected(1);
  // --- queue status -------------------------------------------------------
  // Read-only: what became of one entry, from the entry while it exists and
  // from its history row afterwards, following a successor (tech spec 647 §
  // CLI surface).
  CLI::App* status = queue->add_subcommand(
      "status", "Report what became of a queue entry: its state and place in the queue, how it ended, the exit status, where "
                "its output was saved and, for a cancelled entry, who cancelled it. Reads only; changes nothing.");
  status->add_option("seq")
      ->description("The sequence number, the first line of the ticket `queue run --detach` prints")
      ->required();
  status->add_flag("--json")->description("Print one JSON object with the documented fields, null where a field does not apply");
}
} // namespace planar::cmd::agent::handlers::queue_cli
