/// @file agentdb.cppm
/// @brief `planar.db.agentdb` — the agent database's open path (plan 1080,
/// decision 1181, tasks hq-agentdb-open and hq-agentdb-compat). Resolves
/// the store's location from `PLANAR_AGENT_DB`, falling back to
/// `$HOME/.planar/agent.db` beside the main database, creates the file and
/// its parent directory on first use, opens it with the same WAL journal
/// mode and busy timeout every read-write `planar.db` connection gets,
/// checks that the store does not need a newer binary, and applies the
/// embedded agent migration stream (`planar::db::agent::migrations()`
/// against `k_agent_version_table`) so the store is at head when
/// `open_agent_db` returns.
///
/// The compatibility check (`check_compat`) runs inside the open path,
/// between opening the file and applying any migration, so no caller can
/// open a store and forget it, and a refused store is left exactly as it
/// was. The row with the highest `version` in `agent_schema_migrations` is
/// authoritative: the store is refused only when that row's `compat` is
/// higher than this binary's agent schema version (`agent_schema_version`,
/// the head of the embedded chain). A store ahead of the binary whose
/// `compat` is not is opened as it is.
///
/// Every READ path over the store must tolerate a store behind head.
/// `open_agent_db_read_only_at` never migrates, so after an upgrade a
/// read-only reader (`queue status`) keeps seeing the older schema until some
/// read-write open brings the store to head, and older binaries keep writing
/// it meanwhile. A column an additive migration added reads as NULL (unknown)
/// on such a store, never as a query failure; see
/// `planar.engine.hostqueue.queue::limit_columns_select` for the agent
/// migration 00003 columns. Write paths need no such care: they run only on a
/// connection `open_agent_db` has migrated to head.
///
/// The open path never reads `PLANAR_DB` and never opens the main
/// database: a caller whose main database is locked out by a schema
/// mismatch still opens the agent store (tech-spec § Open Questions "Where
/// the queue store lives").
///
/// Every failure surfaces as `std::expected<..., open_error>`; nothing
/// throws across the module boundary. The error names the path it failed
/// on, so a verb can print it and map the failure to its own exit code
/// (decision 1185: `queue run` exits 125 on an unreachable store).
///
/// File modes (decision 1210): the store holds task claim tokens, so it is
/// created owner-only (0600, sidecars included) and the directories the open
/// path creates are 0700; `$HOME/.planar` is ensured 0700 at the default
/// location. An existing store keeps its mode, which `install.sh` tightens.
///
/// The environment is injected as an `env_lookup` rather than read from
/// the process, so tests pass a fixed table and never mutate the process
/// environment; `process_env()` is the production lookup.

module;

export module planar.db.agentdb;

import std;
import planar.db;

namespace planar::db::agent {

/// @brief Look up one environment variable by name. Returns `std::nullopt`
/// when the variable is not set; an empty string when it is set to nothing.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief The production lookup over the process environment (`getenv`).
/// @return A lookup callable that reads the live process environment.
export auto process_env() -> env_lookup;

/// @brief A deterministic lookup over a fixed table, for tests and for
/// callers that already hold a snapshot of the environment.
/// @param vars The variables the lookup answers; anything else is unset.
/// @return A lookup callable over `vars`.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;

/// @brief The environment variable that overrides the agent database path.
export constexpr std::string_view k_agent_db_env = "PLANAR_AGENT_DB";

/// @brief The variable the fallback location is built from.
export constexpr std::string_view k_home_env = "HOME";

/// @brief What went wrong while opening the agent database.
export enum class open_error_kind : std::uint8_t {
  unresolved_path,     ///< Neither `PLANAR_AGENT_DB` nor `HOME` is set or non-empty, or `PLANAR_AGENT_DB` is empty.
  unwritable_location, ///< The store's parent directory could not be created.
  open_failed,         ///< SQLite could not open or create the file, or could not read its version table.
  incompatible_store,  ///< The store's highest `compat` is above this binary's agent schema version.
  migrate_failed,      ///< The agent migration stream failed to apply.
};

/// @brief The failure `resolve_agent_db_path` / `open_agent_db` report.
///
/// `message` is complete on its own: it names the path (or, for
/// `unresolved_path`, the missing variables) and the underlying reason, so
/// a caller can print it verbatim. `path` is empty only for
/// `unresolved_path`. `store_compat` and `binary_version` are set only for
/// `incompatible_store`, where the message also spells both out.
export struct open_error {
  open_error_kind       kind = open_error_kind::unresolved_path; ///< Which step failed.
  std::filesystem::path path;                                    ///< The store path the failure is about.
  std::string           message;                                 ///< A complete, printable description.
  int                   sqlite_code    = 0;                      ///< The SQLite extended result code, when SQLite failed.
  std::uint32_t         store_compat   = 0;                      ///< The refused store's highest-row `compat`.
  std::uint32_t         binary_version = 0;                      ///< This binary's agent schema version.
};

/// @brief This binary's agent schema version: the highest version in the
/// embedded agent migration chain. A store's `compat` is compared against
/// it.
/// @return The head of `planar::db::agent::migrations()`.
export auto agent_schema_version() -> std::uint32_t;

/// @brief Resolves where the agent database lives: `PLANAR_AGENT_DB` when
/// set and non-empty, else `$HOME/.planar/agent.db` when `HOME` is set and
/// non-empty. `PLANAR_DB` is never consulted; the two stores are
/// independent files.
/// @param env The environment to read.
/// @return The resolved path, or an `unresolved_path` error naming the
/// variables that would have supplied one.
export auto resolve_agent_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, open_error>;

/// @brief Checks that the store on `conn` may be opened by this binary.
/// Reads the row with the highest `version` in `agent_schema_migrations`
/// and refuses only when its `compat` is higher than
/// `agent_schema_version()`. A store without the table, or with an empty
/// one, is fresh or behind and passes. Reads only; never writes.
/// @param conn An open connection to the store.
/// @param path The store's location, for the error message.
/// @return Success, an `incompatible_store` error naming `path`, the
/// store's `compat` and the binary's version, or an `open_failed` error
/// when the version table could not be read.
export auto check_compat(connection& conn, const std::filesystem::path& path) -> std::expected<void, open_error>;

/// @brief Opens the agent database at an explicit path, creating the file
/// and its parent directory when absent, refuses it when `check_compat`
/// does, and otherwise brings it to the head of the embedded agent
/// migration stream. Idempotent: an up-to-date store is opened without any
/// write beyond SQLite's own journal-mode handshake. A refused store is
/// not written at all.
///
/// File modes (decision 1210): a file this call creates is 0600, and so are
/// the `-wal` and `-shm` SQLite derives from it; any parent directory this
/// call creates is 0700. An existing file or directory keeps its mode.
/// @param path The store's location.
/// @return An open read-write connection at the current agent schema
/// version (or at the store's own higher, compatible version), or the
/// failure naming `path`.
export auto open_agent_db_at(const std::filesystem::path& path) -> std::expected<connection, open_error>;

/// @brief Opens the agent database at an explicit path for READING ONLY:
/// strictly read-only at the SQLite layer (`file:<path>?mode=ro` and
/// `SQLITE_OPEN_READONLY`, as `planar-watch` opens the main database), so no
/// statement run on the returned connection can write. Unlike
/// `open_agent_db_at` it never creates the file or its directory, never sets
/// the journal mode and never applies a migration: a store behind the head of
/// the embedded chain is read as it is. It refuses a store `check_compat`
/// refuses, which reads only.
/// @param path The store's location.
/// @return An open read-only connection, an `open_failed` error naming `path`
/// when the file does not exist or cannot be opened, or the `check_compat`
/// failure.
export auto open_agent_db_read_only_at(const std::filesystem::path& path) -> std::expected<connection, open_error>;

/// @brief Resolves the store's path from `env` (see `resolve_agent_db_path`)
/// and opens it read-only (see `open_agent_db_read_only_at`).
/// @param env The environment to read.
/// @return An open read-only connection, or the first failure.
export auto open_agent_db_read_only(const env_lookup& env) -> std::expected<connection, open_error>;

/// @brief Resolves the store's path from `env` (see `resolve_agent_db_path`)
/// and opens it (see `open_agent_db_at`). When the path is the default
/// `$HOME/.planar/agent.db` (no `PLANAR_AGENT_DB`), the directory
/// `$HOME/.planar` is also ensured 0700 if the current user owns it; a
/// directory named through `PLANAR_AGENT_DB` is never chmodded.
/// @param env The environment to read.
/// @return An open connection at the current agent schema version, or the
/// first failure.
export auto open_agent_db(const env_lookup& env) -> std::expected<connection, open_error>;

} // namespace planar::db::agent
