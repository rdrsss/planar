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
  // The foreground form. `--detach`, `--timeout`, `--wait-timeout`,
  // `--vendor`, `--role`, `--claim` and `--notices` are declared by the
  // tasks that implement them (tech spec 647 § CLI surface).
  CLI::App* run = queue->add_subcommand(
      "run", "Run a command in turn, host-wide: wait for the command's turn in the queue, run it in the caller's directory "
             "with the caller's environment, and exit with its status.");
  run->add_option("--label")->description("Operator-facing label recorded with the queue entry");
  run->add_option("command")->description("The command and its arguments, after `--`")->required()->expected(1, -1);
}
} // namespace planar::cmd::agent::handlers::queue_cli
