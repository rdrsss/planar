/// @file path.cppm
/// @brief `planar.buildqueue.path` — the layer-1 resolver for the host build
/// queue's database and its detached-run log directory (plan 1163, decision
/// 1438).
///
/// Entry points: `resolve_queue_db_path(env)` picks the database file, and
/// `queue_log_directory(db_path)` names the directory that holds detached-run
/// output beside it. The environment names are `k_env_db` and `k_env_slot`.
///
/// The database is `$PLANAR_BUILD_QUEUE_DB` when that is set and non-empty,
/// else `$HOME/.planar/build-queue.db`. An empty value counts as unset. With
/// neither variable usable the resolver returns `error::no_location`, and
/// `error_message` names both variables. The log directory is the database's
/// path with a trailing `.db` removed and `-logs` appended, beside the file, so
/// the default is `$HOME/.planar/build-queue-logs/`.
///
/// Error boundary: every fallible call returns `std::expected`, and nothing
/// here throws, opens a file or reads the process environment. The lookup is
/// injected, so a test drives the resolver with a map and never sets an
/// environment variable. Layer 1: this module imports only `std`.
module;

export module planar.buildqueue.path;

import std;

namespace planar::buildqueue {

/// @brief Look up one environment variable. An absent variable is `nullopt`.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Environment variable that overrides the queue database path.
export inline constexpr std::string_view k_env_db = "PLANAR_BUILD_QUEUE_DB";

/// @brief Environment variable that carries an entry's sequence number to the
/// command it runs, so a nested `queue run` can find its parent.
export inline constexpr std::string_view k_env_slot = "PLANAR_BUILD_QUEUE_SLOT";

/// @brief Error surface for the path resolver.
export enum class error : std::uint8_t {
  no_location, ///< Neither `PLANAR_BUILD_QUEUE_DB` nor `HOME` is set and non-empty.
};

/// @brief One line that names the failure and the variables to set.
/// @param value The error to describe.
/// @return A message that names both `PLANAR_BUILD_QUEUE_DB` and `HOME`.
export auto error_message(error value) -> std::string;

/// @brief Resolve the build queue database path.
/// @param env Environment lookup; an empty value counts as unset.
/// @return `PLANAR_BUILD_QUEUE_DB` when set, else `HOME/.planar/build-queue.db`,
/// or `error::no_location` when neither is usable.
export auto resolve_queue_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, error>;

/// @brief The directory that holds detached-run output for a queue database.
/// Pure: it names the directory and creates nothing.
/// @param db_path The database file path.
/// @return `db_path` without its `.db` suffix, plus `-logs`, in the same
/// parent directory (`/x/q.db` gives `/x/q-logs`).
export auto queue_log_directory(const std::filesystem::path& db_path) -> std::filesystem::path;

} // namespace planar::buildqueue
