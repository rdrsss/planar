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
/// ## Time limits
///
/// `--timeout <duration>` (default 30 minutes) is the run limit and
/// `--wait-timeout <duration>` (default none) the wait limit. Both take the
/// `[queue]` duration grammar (an integer and `ms`, `s`, `m` or `h`), share
/// its 24-hour cap, and refuse zero, negative and unit-less values at exit 2
/// before anything is enqueued. The run limit becomes the entry's
/// `deadline_mono` when the entry starts, and is recorded as `run_limit_ms`
/// in the same statement; the wait limit is stored as `wait_deadline_mono`,
/// and as `wait_limit_ms`, at enqueue (`queue status` reports both). At its
/// run limit the submitter marks its own entry terminating with reason `timeout` (SIGTERM) and advances that
/// entry each tick (SIGKILL after the `[queue]` grace period). The entry then
/// ends with the outcome its stop reason names, never `signaled`: `timeout`
/// (exit 124) or `cancelled` (exit 125), whichever process set the reason.
/// A wait limit reached before the turn removes the entry with outcome
/// `wait_timeout` and exits 125 without running the command.
///
/// ## Signals
///
/// The handler catches SIGINT, SIGTERM and SIGHUP from just before the enqueue
/// to its return, through a self-pipe: the handler only writes the signal
/// number, and the wait loops act on it. While waiting, a signal removes the
/// entry (outcome `cancelled`, attributed to the submitter's pid), runs nothing
/// and exits 125 (decision 1188: cancelled). While the command runs, it is forwarded to
/// the command's process group and supervision goes on; nothing escalates, so a
/// command that survives the signal keeps its slot and the outcome and exit
/// code are what the command did (`signaled`, 128 plus N, or `exited` with its
/// own code). A signal ignored at start stays ignored. Only the recorded child
/// group is signalled, through `runner::signal`.
///
/// ## Nested runs
///
/// The command runs with `PLANAR_QUEUE_SLOT` set to its entry's sequence
/// number. A `queue run` that finds the variable set to the sequence number of
/// a live running entry (`engine::hostqueue::enqueue_nested`) runs as a nested
/// entry: inserted `running` with `parent_seq` set, outside the slot count and
/// arrival order, with its own deadline (the run limit of its own `--timeout`
/// or the default, not the parent's) and its own history row marked nested.
/// A wait limit has nothing to bound and is ignored. Any other value (not a
/// positive integer, no such entry, an entry still waiting, an entry that is
/// not live) queues normally. The marker is advisory and the parent may end
/// first; the nested entry is then supervised and ended like any other. A
/// nested entry's own command sees its own sequence number, so a third level
/// nests under the second.
///
/// When the store stays busy past its timeout while the nested entry is being
/// inserted, the insert is retried at the poll interval for as long as the
/// staleness window, then refused at 125 with the command not run. It is
/// neither queued normally (it would wait behind the entry that is waiting for
/// it) nor run unqueued (nobody checked the marker). Any other store failure
/// refuses at once.
///
/// ## Missing entry
///
/// A submitter that finds its own entry gone reads the history row for its
/// sequence number (tech spec 647 § Waiting and claiming a turn).
///
/// A WAITING submitter: `abandoned` (it was reaped while stopped) puts it back
/// at the back of the queue. `engine::hostqueue::rejoin` inserts a new entry
/// (a higher sequence number, so behind everything that arrived meanwhile) and
/// records its number in the old row's `successor_seq`, in one transaction, so
/// `queue status` can follow the chain. The new entry keeps the original wait
/// limit's deadline, so `--wait-timeout` bounds the whole wait, not each try.
/// A rejoin (or the history read after the third) that finds the store busy is
/// retried at the poll interval for as long as the staleness window, counted
/// from the FIRST failure and cleared only by a success, then refused at 125
/// with the command not run; any other store failure refuses at once (the same
/// rule as the nested insert). A submitter rejoins at most three times and then exits 125: the spec names
/// no bound, and one reaped after every rejoin would otherwise queue for ever.
/// `cancelled`, any other outcome, or no row at all (never written, or pruned)
/// exits 125 without running the command: the entry ended by a path the
/// submitter did not take.
///
/// A RUNNING submitter keeps supervising its command, never rejoins, never
/// runs the command a second time, and exits with what it observed of the
/// child. The run limit is a property of the entry (the submitter marks its
/// entry terminating), so with the entry gone it is no longer enforced; the
/// submitter says so once on standard error. A store that cannot be reached
/// changes nothing.
///
/// ## Give-up
///
/// A waiting submitter that cannot complete any poll for longer than the
/// staleness window ends its own entry as `abandoned` and exits 125 (task
/// hq-giveup-history-fields). The row is the one of an entry that never ran:
/// no exit code, signal, start time or run time, no successor (it does not
/// rejoin) and no canceller; it keeps the command as submitted and
/// `waited_ms` runs to the end. That is the spec's `abandoned` (no longer
/// live), and the engine's `end_entry` refuses any of those fields on it.
///
/// ## Known limits
///
/// A queued command cannot read the terminal. It runs in its own process group,
/// never the terminal's foreground group, so a terminal read stops it with
/// SIGTTIN. The handler neither hands the terminal over nor redirects standard
/// input: queued commands are non-interactive builds and tests (docs/
/// cli-reference.md, "Known limits").
///
/// ## What is not here yet
///
/// The command guard and the 126/127 checks run first (tasks hq-command-guard
/// and hq-not-started): a launcher exits 2, a missing program 127 and one that
/// cannot be executed 126, each before the configuration or the store is
/// touched, so nothing is enqueued. A program that cannot start at its turn ends
/// its entry `not_started` with the same 127 or 126. A later task of the same
/// milestone adds `--claim`; this handler leaves it out.
///
/// ## Notices
///
/// `--notices` (task hq-notices) writes `queue: entry <seq> <what happened>`
/// lines to standard error: `waiting at position <n>` (when the entry first
/// waits and whenever its place changes; a rejoined entry says it again under
/// its new number), `started`, and a last line with the outcome
/// (`exited with code <n>`, `terminated by signal <n>`, `stopped at its run
/// limit`, `cancelled`, `cancelled before its turn`, `removed at its wait
/// limit` or `not started`; a path that ends without a command outcome says why, e.g. `ended as exited without this submitter`).
/// Every exit after the entry exists writes one, after its `error: queue:` line. Standard output is never written by the queue,
/// with or without the flag, and without it the queue writes nothing to
/// standard error on the happy path. The `warning: queue:` diagnostics of a
/// degraded path and the `error: queue:` lines are written either way.
///
/// ## Detached runs
///
/// `--detach` (task hq-detach; tech spec 647 § Submitting, With `--detach`)
/// runs the same submitter in a forked child. The invoked process refuses
/// what it can on its own (the guard, 126/127, the duration flags) and then
/// creates a pipe and forks BEFORE the configuration is read or the store is
/// opened, so no store handle or thread exists at the fork. The child starts
/// a session, closes every inherited descriptor but the pipe, reads
/// `/dev/null`, inserts its entry, creates `<agent-db-directory>/queue-logs/
/// <seq>.log` (`0600`), points standard output and error at it and only then
/// writes the ticket to the pipe: `ok`, the sequence number and the path, or
/// `err` and the error text. Until the log exists the child's standard error
/// is a private buffer, so every refusal it would have printed is the message
/// the invoked process prints; it exits 125 with it. End of file with nothing
/// written (the child died) is 125 with "no ticket was issued". A log that
/// cannot be created, or a ticket that cannot be delivered, takes the entry
/// back out with `engine::hostqueue::discard_entry` and writes no history row.
/// The child never returns to the caller's stack: it leaves with `_exit`. The
/// invoked process prints the sequence number and the path, one per line, and
/// exits 0. `queue_run_deps::detach_hook` is the test seam for the stages.
///
/// ## Vendor and role
///
/// `--vendor` and `--role` (task hq-vendor-role) name the submitting agent.
/// Each is the flag when given and not empty, else `$PLANAR_VENDOR` or
/// `$PLANAR_ROLE` when set and not empty, else stored empty (SQL NULL). The
/// values reach the entry, its history row, a rejoined entry and a nested
/// entry.
module;

export module planar.cmd.planar_agent.handlers.queue;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
import planar.db;
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
  /// @brief Sleeps for a duration; when empty, waits on the signal relay, so a
  /// forwarded signal ends the wait early.
  std::function<void(std::chrono::milliseconds)> sleep;
  /// @brief The nested insert: what `engine::hostqueue::enqueue_nested` does
  /// with its arguments. A test replaces it to make the store report busy.
  using nested_enqueuer = std::function<std::expected<engine::hostqueue::nested_result, engine::hostqueue::queue_error>(
      db::connection&, std::int64_t, const engine::hostqueue::enqueue_request&, const engine::hostqueue::nested_limits&,
      process::identity::clock&, const engine::hostqueue::process_probe&)>;
  /// @brief Inserts a nested entry; `engine::hostqueue::enqueue_nested` when empty.
  nested_enqueuer enqueue_nested;
  /// @brief The rejoin of a reaped waiter: what `engine::hostqueue::rejoin`
  /// does with its arguments. A test replaces it to make the store fail.
  using rejoiner = std::function<std::expected<engine::hostqueue::rejoin_result, engine::hostqueue::queue_error>(
      db::connection&, std::int64_t, const engine::hostqueue::enqueue_request&)>;
  /// @brief Rejoins the queue; `engine::hostqueue::rejoin` when empty.
  rejoiner rejoin;
  /// @brief A test seam for `--detach`, called with a stage name: `before_fork`
  /// in the invoked process, then `after_setsid`, `after_insert` and
  /// `before_report` in the detached child. It runs in whichever process
  /// reaches the stage, so a hook that calls `_exit` makes the child die at
  /// that point. Empty in production.
  std::function<void(std::string_view)> detach_hook;
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
