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
  // The foreground form. `--detach`, `--vendor`, `--role`, `--claim` and
  // `--notices` are declared by the tasks that implement them (tech spec
  // 647 § CLI surface).
  CLI::App* run = queue->add_subcommand(
      "run", "Run a command in turn, host-wide: wait for the command's turn in the queue, run it in the caller's directory "
             "with the caller's environment, and exit with its status.");
  run->add_option("--timeout")
      ->description(
          "How long the command may run before it is stopped (SIGTERM, then SIGKILL after the grace period): an integer "
          "and a unit, ms, s, m or h, at most 24h; default 30m");
  run->add_option("--wait-timeout")
      ->description(
          "How long the entry may wait for its turn before it is removed without running: an integer and a unit, ms, s, m "
          "or h, at most 24h; default no limit");
  run->add_option("--label")->description("Operator-facing label recorded with the queue entry");
  run->add_option("command")->description("The command and its arguments, after `--`")->required()->expected(1, -1);
}
} // namespace planar::cmd::agent::handlers::queue_cli
