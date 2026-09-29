/// @file runner.cppm
/// @brief `planar.process.runner` — the command runner the host build and
/// test queue starts a queued command with (plan 1080, task 7006).
///
/// The queue's foreground `queue run` (tech spec artifact 647, § Running and
/// § Stopping a command) needs five things from a child process that
/// `planar.process::run_inherited` deliberately does not give:
///
///   * **Resolution that tells missing from not executable.** `resolve`
///     follows `PATH` lookup semantics for a bare name and uses a name that
///     contains `/` as given. It reports `error::not_found` and
///     `error::not_executable` as different values, so the queue can refuse
///     a command before enqueueing it and exit 127 or 126 accordingly.
///   * **A new process group.** `start` puts the child in a group of its
///     own, so stopping the command reaches everything it started and
///     nothing the caller did.
///   * **The caller's environment plus one variable.** The queue passes
///     `PLANAR_QUEUE_SLOT`; everything else is inherited unchanged, as are
///     the three standard streams.
///   * **A wait that does not block.** `poll` reports running, exited with
///     a status, or terminated by a signal. A signal is never folded into an
///     exit status; the queue maps signal N to 128 + N itself.
///   * **Group signalling.** `signal` delegates to
///     `planar.process.identity::signal_group`, which refuses group ids 0
///     and 1, so no call here can reach `kill(0, ...)` or `kill(-1, ...)`.
///
/// `run_inherited` is not changed by this module: it collapses death by
/// signal to exit 1 and cannot tell a missing program from one that is not
/// executable, and its callers depend on both.
///
/// Every fallible call returns `std::expected<T, error>`. No errno and no
/// exception crosses this boundary, and the C calls are confined to the
/// implementation unit.
module;

export module planar.process.runner;

import std;
import planar.process;
import planar.process.identity;

namespace planar::process::runner {

/// @brief What an operation in this module can fail with.
export enum class error : std::uint8_t {
  empty_command,            ///< The argument vector was empty, or its program name was empty.
  not_found,                ///< Nothing by the program's name exists (the queue exits 127).
  not_executable,           ///< The program exists but cannot be executed (the queue exits 126).
  invalid_environment,      ///< The added variable's name is empty or contains `=`.
  working_directory_failed, ///< The child could not change to the requested working directory.
  spawn_failed,             ///< A pipe, fork or group placement failed, or exec failed for another reason.
  start_time_failed,        ///< The child's start time could not be read; the child was killed and reaped.
  wait_failed,              ///< `waitpid` failed: the child was already reaped or is not this process's child.
  no_such_process,          ///< A signal was sent to a group that has no member.
  not_permitted,            ///< A signal was refused by the kernel.
  invalid_signal,           ///< The signal number is not valid on this host.
  signal_failed,            ///< A signal could not be sent for another reason.
};

/// @brief Resolve `program` the way `execvp` would, before anything is
/// spawned.
///
/// A name containing `/` is used as given. A bare name is searched along
/// the `PATH` that `env` reports, in order, skipping empty elements; the
/// first regular file the caller may execute wins. A name that matched
/// only files that are not executable, or directories, is
/// `not_executable`; a name that matched nothing, or a lookup with no
/// `PATH`, is `not_found`.
/// @param env The environment lookup, consulted for `PATH`.
/// @param program The program name or path.
/// @return The path to execute, or `empty_command`, `not_found` or
/// `not_executable`.
export auto resolve(const env_lookup& env, std::string_view program) -> std::expected<std::string, error>;

/// @brief How to start a command.
export struct start_options {
  /// @brief The working directory for the child; unset inherits the
  /// caller's.
  std::optional<std::filesystem::path> working_directory;
  /// @brief The name of the one variable added to the inherited
  /// environment. An existing variable of that name is replaced.
  std::string env_name;
  /// @brief The added variable's value.
  std::string env_value;
};

/// @brief A started child: its process id, its process group id, and the
/// start time that identifies this incarnation of the pid.
///
/// The group id equals the process id, because the child leads its own
/// group. The start time is read while the child is known to be alive and
/// not yet reaped, so it always names this child.
export struct child {
  std::int64_t         pid  = 0;  ///< The child's process id.
  std::int64_t         pgid = 0;  ///< The child's process group id.
  identity::start_time started{}; ///< The child's start time, for `child_started`.
};

/// @brief Start `argv` as a child in a new process group.
///
/// `argv[0]` is resolved with `resolve` before anything is forked, so a
/// missing or unexecutable program is reported without a child. The child
/// is placed in its own group by both the parent and the child, so neither
/// can observe it in the caller's group. It gets the caller's environment
/// with `options.env_name` set to `options.env_value`, the caller's
/// standard streams, and `options.working_directory` when given. `argv[0]`
/// is passed to the program as given; the resolved path is what is
/// executed. Signal handlers the caller installed are reset to the default
/// in the child before it executes, so a signal that arrives in that window
/// acts as it would on the program; ignored signals stay ignored.
///
/// A failure after the fork (changing directory, or an `exec` that fails
/// because the program vanished or lost its permission since `resolve`) is
/// reported as the error it maps to, and the child has been reaped.
/// @param env The environment lookup, for resolving `argv[0]` along `PATH`.
/// @param argv The full argument vector; `argv[0]` is the program.
/// @param options The working directory and the added variable.
/// @return The started child, or the error that kept it from running.
export auto start(const env_lookup& env, std::span<const std::string> argv, const start_options& options)
    -> std::expected<child, error>;

/// @brief What a poll of a child found.
export enum class state : std::uint8_t {
  running,   ///< The child has not terminated.
  exited,    ///< The child exited; `code` is its exit status.
  signalled, ///< The child was terminated by a signal; `code` is the signal number.
};

/// @brief The result of `poll`.
export struct status {
  state kind = state::running; ///< Whether and how the child terminated.
  int   code = 0;              ///< The exit status or signal number; 0 while running.
};

/// @brief Check on `target` without blocking.
///
/// A child that has terminated is reaped by the call that reports it, so
/// its pid is released; polling it again is `wait_failed`.
/// @param target A child returned by `start` and not yet reported as
/// terminated.
/// @return Running, exited with a status, or signalled with a signal
/// number; or `wait_failed`.
export auto poll(const child& target) -> std::expected<status, error>;

/// @brief Send `sig` to every member of `target`'s process group.
///
/// Delegates to `planar.process.identity::signal_group`, which refuses
/// group ids 0 and 1.
/// @param target The child whose group is signalled.
/// @param sig The signal number, as the platform defines it.
/// @return Success, or `no_such_process`, `not_permitted`,
/// `invalid_signal` or `signal_failed`.
export auto signal(const child& target, int sig) -> std::expected<void, error>;

} // namespace planar::process::runner
