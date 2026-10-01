/// @file queue_store.cppm
/// @brief `planar.cmd.planar_agent.queue_store` — how the `planar-agent queue`
/// verbs open the queue's store, which is `planar.db` (plan 1089, tasks
/// qp-agent-queue-open and qp-agent-queue-tests; tech spec 656 § Store and
/// open path; decisions 1219-1221).
///
/// The `queue` domain is exempt from `main.cpp`'s pre-dispatch path check, so
/// no refusal happens before a handler runs, and every queue handler that
/// needs the store calls `open_queue_store` itself:
///
///  1. the path is resolved with the binary's own `resolve_db_path`
///     (`PLANAR_DB`, else `$HOME/.planar/planar.db`); neither set is a refusal;
///  2. the file is opened without ever creating it: read-write with a bounded
///     lock wait (`db::connection::open_existing`) for `queue run` and `queue
///     cancel`, read-only at the SQLite layer (`open_read_only`) for `queue
///     status`; a missing file is a refusal that names `planar init`;
///  3. the `schema_migrations` handshake (`db::assert_schema_compatible`) runs
///     FIRST, with its diagnostics captured into the one refusal line and never
///     written, so a behind database is `schema_version_behind` before the
///     queue's own check is consulted;
///  4. at an equal or ahead version `hostqueue::check_queue_schema` decides
///     whether this binary may use the queue tables, and its failure is
///     `queue_schema_foreign` (equal) or `queue_schema_incompatible` (ahead).
///
/// Every refusal is a `store_refusal`: a `--json` tag and ONE printable
/// reason-and-remedy sentence. Both the tag and the sentence are what the
/// handlers print and what the installer's probe parses, so neither leaks a
/// second line. Nothing here writes to the database or throws.
///
/// The detached-run log directory is a function of the database FILE NAME
/// (`queue_log_directory`), so two databases in one directory never share a
/// counter-restart collision on `1000001.log`.
module;

export module planar.cmd.planar_agent.queue_store;

import std;
import planar.db;
import planar.cmd.planar_agent.context;

namespace planar::cmd::agent {

/// @brief How a queue verb needs the store.
export enum class store_access : std::uint8_t {
  read_write, ///< `queue run` and `queue cancel`: `open_existing`, a bounded lock wait.
  read_only,  ///< `queue status`: strictly read-only at the SQLite layer.
};

/// @brief The `--json` tag of a refusal that cannot name the store (no path, a
/// missing file, a file that is not a database, an unopenable path).
export constexpr std::string_view k_tag_store_unreachable = "store_unreachable";

/// @brief The `--json` tag of a refusal where the store exists but a query
/// against it failed (a lock past the bound, an unreadable version).
export constexpr std::string_view k_tag_store_unreadable = "store_unreadable";

/// @brief The `--json` tag of a database that is behind this binary.
export constexpr std::string_view k_tag_schema_version_behind = "schema_version_behind";

/// @brief Why a queue verb could not use the store.
export struct store_refusal {
  std::string tag;     ///< The `--json` tag; one of the `k_tag_*` values or the hostqueue schema tags.
  std::string message; ///< One sentence, `<reason>; <remedy>`, with no newline and no `error:` prefix.
};

/// @brief An open store: the connection and the path it was opened from.
export struct queue_store {
  db::connection        conn; ///< The open connection, read-only exactly when `store_access::read_only` was asked for.
  std::filesystem::path path; ///< The resolved database path.
};

/// @brief Resolves, opens and vets the queue's store for one verb. Never
/// creates `planar.db`, never migrates, and never writes; see this module's
/// description for the order of the checks.
/// @param env The invocation's environment.
/// @param access Whether the verb writes.
/// @return The open store, or the refusal naming the tag and the remedy.
export auto open_queue_store(const env_lookup& env, store_access access) -> std::expected<queue_store, store_refusal>;

/// @brief The directory a detached run's `<seq>.log` files go in, for the
/// database at `db_path`: `<dir>/queue-logs/` when the file is named
/// `planar.db`, else `<dir>/<stem>.queue-logs/`, where the stem is the file
/// name with its final extension removed, as `std::filesystem::path::stem()`
/// computes it (`a.b.db` gives `a.b`, `queuedb` gives `queuedb`, `.hidden`
/// gives `.hidden`). Pure: touches no file.
/// @param db_path The database file.
/// @return The log directory.
export auto queue_log_directory(const std::filesystem::path& db_path) -> std::filesystem::path;

} // namespace planar::cmd::agent
