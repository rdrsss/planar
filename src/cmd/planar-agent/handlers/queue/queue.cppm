/// @file queue.cppm
/// @brief `planar.cmd.planar_agent.handlers.queue` — the `planar-agent queue
/// run` handler (plan 1080, task hq-queue-run-verb).
///
/// `queue run -- <command>` is the foreground form of the host-wide build and
/// test queue (tech spec 647 § Submitting, Waiting and claiming a turn,
/// Running). The process that submits a command is the process that runs it:
/// it enqueues an entry, polls until the entry's turn, runs the command in
/// the caller's directory with the caller's environment, refreshes the entry
/// while the command runs, removes the entry with its one history row, and
/// exits with the command's status. Everything the queue's own logic needs
/// lives in `planar.engine.hostqueue`; this handler supplies the values that
/// engine takes as arguments (host identity, pid and start time, clocks, the
/// `[queue]` settings) and owns the loop and the exit code.
///
/// ## Exit status
///
/// The handler returns an `exit_status`, so dispatch passes the command's own
/// status through verbatim and writes no `--json` envelope: a command exit of
/// 1, 2 or 125 is never confused with this binary's own refusal of the same
/// number (decision 1188). A signalled command exits 128 plus the signal.
/// `queue run` declares no `--json`, so the carry-forward from task 7007
/// (the pass-through path writes no envelope) has nothing to decide here.
/// The queue's own failures exit 125 with one line on standard error; a
/// parse failure (no command given) never reaches the handler and exits 1.
///
/// ## Configuration
///
/// `slots`, `poll_interval`, `stale_after`, `grace` and `history_days` come
/// only from `planar.engine.config.queue::load_queue_settings`, read once
/// before the enqueue and again at every poll. A configuration that cannot be
/// read before the enqueue refuses at 125, since the store and the
/// configuration are then unusable; one that cannot be read mid-wait keeps
/// the previous settings and is reported once per failure streak. The
/// engine never sees the config module: the handler copies values into the
/// engine's request structs.
///
/// ## What is not here yet
///
/// The command guard and the 126/127 checks run first (tasks hq-command-guard
/// and hq-not-started): a launcher exits 2, a missing program 127 and one that
/// cannot be executed 126, each before the configuration or the store is
/// touched, so nothing is enqueued. A program that cannot start at its turn ends
/// its entry `not_started` with the same 127 or 126. Later tasks of the same
/// milestone add `--timeout` and `--wait-timeout`, signal
/// forwarding, nested runs, missing-entry handling, `--notices`, `--vendor`
/// and `--role`, and `--claim`. This handler leaves them out and says so at
/// each seam.
module;

export module planar.cmd.planar_agent.handlers.queue;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.process.identity;

namespace planar::cmd::agent::handlers {

/// @brief The seams `queue_run_with` reads instead of the process, so a test
/// can drive time, liveness, signalling and configuration by hand. Every
/// member left empty takes the production default.
export struct queue_run_deps {
  /// @brief The monotonic and wall clocks; the system clock when null.
  std::shared_ptr<process::identity::clock> clock;
  /// @brief The process queries liveness uses; `system_process_probe()` when empty.
  std::optional<engine::hostqueue::process_probe> probe;
  /// @brief Where SIGTERM and SIGKILL go; `system_group_signaller()` when empty.
  engine::hostqueue::group_signaller signaller;
  /// @brief Loads the `[queue]` settings; reads the configuration file the
  /// context's environment names when empty.
  std::function<std::expected<engine::config::queue_settings, engine::config::queue_load_error>()> load_settings;
  /// @brief Sleeps for a duration; `std::this_thread::sleep_for` when empty.
  std::function<void(std::chrono::milliseconds)> sleep;
};

/// @brief `planar-agent queue run -- <command>` with the production
/// defaults.
/// @param ctx The invocation context.
/// @param args The parsed arguments: the `command` positional and `--label`.
/// @return The command's exit status, or 125 when the queue failed.
export auto queue_run(context& ctx, const cliapp::parsed_args& args) -> handler_outcome;

/// @brief `queue_run` with its seams supplied.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param deps The clock, probe, signaller, settings loader and sleeper.
/// @return The command's exit status, or 125 when the queue failed.
export auto queue_run_with(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps) -> handler_outcome;

} // namespace planar::cmd::agent::handlers
