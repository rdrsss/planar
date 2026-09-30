/// @file agentstore.cppm
/// @brief `planar.cmd.planar_watch.agentstore` — how `planar-watch` reaches
/// the agent database (plan 1080, tasks 7024 hq-watch-agentdb and 7092, the
/// read tolerance for a store behind head). The viewer's
/// second store, beside the main database `database.cppm` opens, under the
/// same invariant: it opens SQLite strictly read-only and writes nothing.
///
/// The access policy, in one place for the `queue` views:
///  - the path comes from `PLANAR_AGENT_DB`, else `$HOME/.planar/agent.db`
///    (`planar.db.agentdb::resolve_agent_db_path`, shared with `planar-agent`);
///  - the store is opened with `planar.db.agentdb::open_agent_db_read_only_at`
///    (`file:...?mode=ro`, `SQLITE_OPEN_READONLY`): it never creates the
///    database file or its directory, never sets a journal mode and never
///    migrates, so a store behind head is read as it is. SQLite itself may
///    create empty `-wal` / `-shm` sidecars beside an existing WAL store,
///    exactly as for the main database. `immutable=1` would avoid that, but
///    it hides a live writer's uncheckpointed rows, so it is not used;
///  - the agent compatibility check runs at open, and a store whose highest
///    `compat` is above this binary's agent schema version is refused as
///    `schema_version_ahead` (exit 7), naming both values;
///  - a MISSING store is a valid, empty queue: nothing has ever been
///    submitted. `open_agent_store` succeeds with a store whose `present()` is
///    false and whose reads are empty. Any other open failure (an unreadable
///    file, something that is not a SQLite database) is an error, never
///    "empty". So is a file that has tables but no `agent_schema_migrations`
///    table (`PLANAR_AGENT_DB` pointed at another database by mistake): it is
///    refused as not an agent store, exit 1. A zero-byte file, or one with no
///    tables at all, is a store created and not yet migrated, and reads empty.
///
/// Reads go through `planar.engine.hostqueue`'s `list` and `list_history`,
/// which select the limit columns agent migration 00003 added through
/// `limit_columns_select`, so a store still at agent schema version 2 reads
/// with those columns null. A store that has no queue tables yet (a file a
/// submitter has created and not yet migrated) reads as empty too.
///
/// Every fallible boundary returns `std::expected<T, domain_error>`.
module;

export module planar.cmd.planar_watch.agentstore;

import std;
import planar.db;
import planar.db.agentdb;
import planar.engine.hostqueue;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {

/// @brief Environment lookup accepted by the agent store's path resolution.
export using agent_env_lookup = db::agent::env_lookup;

/// @brief The viewer's read-only view of the agent database, or the absence
/// of one.
///
/// Invariant: `connection()` is either null (the store does not exist) or a
/// connection opened read-only by `open_agent_db_read_only_at`; no member
/// writes. Not thread-safe; one view per invocation.
export class agent_store {
  std::filesystem::path         _path;
  std::optional<db::connection> _connection;

public:
  /// @brief Hold a store's path and, when it exists, its read-only handle.
  /// @param path The store's location.
  /// @param connection The read-only handle, or empty for a missing store.
  agent_store(std::filesystem::path path, std::optional<db::connection> connection);

  /// @brief The store's resolved location, whether or not it exists.
  /// @return Borrowed path.
  [[nodiscard]] auto path() const -> const std::filesystem::path&;

  /// @brief Whether the store exists. False means an empty queue, not a fault.
  /// @return True when a read-only handle is held.
  [[nodiscard]] auto present() const -> bool;

  /// @brief The read-only handle, for a caller that needs SQL of its own.
  /// @return The handle, or null when the store is missing.
  [[nodiscard]] auto connection() -> db::connection*;

  /// @brief Every running and waiting entry, lowest sequence number first.
  /// Empty for a missing or not-yet-migrated store. Limit columns read null on
  /// a store below agent schema version 3.
  /// @return The entries, or the failure.
  [[nodiscard]] auto entries() -> std::expected<std::vector<engine::hostqueue::entry>, domain_error>;

  /// @brief Ended entries, in the order `hostqueue::list_history` returns.
  /// Empty for a missing or not-yet-migrated store.
  /// @param ended_since Only rows that ended at or after this wall-clock
  /// instant (ms since the epoch), when given.
  /// @return The history rows, or the failure.
  [[nodiscard]] auto history(std::optional<std::int64_t> ended_since = std::nullopt)
      -> std::expected<std::vector<engine::hostqueue::history_row>, domain_error>;
};

/// @brief Maps an agent database open failure to this binary's error:
/// `incompatible_store` becomes `schema_version_ahead` (exit 7) and every
/// other failure `generic_failure` (exit 1). The message is the open error's
/// own, which names the path and, for a refused store, both versions.
/// @param error The open failure.
/// @return The command error.
export auto agent_open_error(const db::agent::open_error& error) -> domain_error;

/// @brief Opens the agent store at an explicit path under the policy above.
/// @param path The store's location.
/// @return The view (present or missing), or `schema_version_ahead` for an
/// incompatible store, or `generic_failure` for any other open failure.
export auto open_agent_store_at(const std::filesystem::path& path) -> std::expected<agent_store, domain_error>;

/// @brief Resolves the path from `env` (`PLANAR_AGENT_DB`, else
/// `$HOME/.planar/agent.db`) and opens it under the policy above. Never reads
/// `PLANAR_DB` and never opens the main database.
/// @param env The environment to read.
/// @return The view, or the failure; an unresolvable path is `generic_failure`.
export auto open_agent_store(const agent_env_lookup& env) -> std::expected<agent_store, domain_error>;

} // namespace planar::cmd::watch
