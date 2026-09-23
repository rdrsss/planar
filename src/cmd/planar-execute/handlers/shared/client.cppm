/// @file client.cppm
/// @brief Centurion client operations shared by planar-execute command families.
module;
export module planar.cmd.planar_execute.handlers.shared.client;
import std;
import planar.cmd.planar_execute.cli;

export namespace planar::cmd::execute::handlers::client {
/// @brief Submit a run to the profile's daemon and follow its result.
/// @param asked Parsed submit arguments.
/// @return Process exit code.
auto submit_run(const submit_args& asked) -> int;

/// @brief Show a run or the profile's daemon status.
/// @param asked Parsed status arguments.
/// @return Process exit code.
auto status_run(const run_id_args& asked) -> int;

/// @brief Request cancellation of one run.
/// @param asked Parsed cancellation arguments.
/// @return Process exit code.
auto cancel_run_verb(const run_id_args& asked) -> int;

/// @brief Show the daemon serving a profile.
/// @param asked Parsed host arguments.
/// @return Process exit code.
auto host_status(const run_id_args& asked) -> int;

/// @brief Drain or stop the daemon serving a profile.
/// @param action Lifecycle action to perform.
/// @param asked Parsed host arguments.
/// @return Process exit code.
auto host_lifecycle(std::string_view action, const run_id_args& asked) -> int;

/// @brief Follow one run's events from a stored or explicit cursor.
/// @param asked Parsed follow arguments.
/// @return Process exit code.
auto follow_run_verb(const run_id_args& asked) -> int;
} // namespace planar::cmd::execute::handlers::client
