/// @file reader.cppm
/// @brief `planar.buildqueue.reader` — the strictly read-only reader of the
/// host build queue's database, for Planar's gate evidence and health checks
/// (decision 1441).
///
/// Entry points: `read_tagged_runs` returns the runs carrying one tag, with
/// the database's creation time and tagged-history retention. `read_queue_facts`
/// returns the same header without runs. `open_reader` is the one open sequence
/// both share: `open_existing` with a 2 second busy bound, then
/// `pragma query_only = on` before any read, so the connection can change no row.
///
/// Error boundary: a missing file is `reader_state::absent`, and nothing is ever
/// created. A database still locked after the busy bound is `reader_error_kind::busy`.
/// One that cannot be opened or read is `unreadable`. One without the identity
/// row or the columns the reader reads is `incompatible`, and the reason names
/// what is missing. Versions are not compared: a database behind or ahead of
/// this build is read when its columns exist. Every failure is returned through
/// `std::expected`, and nothing throws. Layer 1: the interface imports only `std`
/// and `planar.db`; the implementation also imports `planar.json_dom` to decode
/// the stored argv.
module;

export module planar.buildqueue.reader;

import std;
import planar.db;

namespace planar::buildqueue {

/// @brief Whether the database file exists. `absent` is a normal state, not a failure.
export enum class reader_state : std::uint8_t {
  present, ///< The file exists and its identity and columns were read.
  absent,  ///< No file at the path. Nothing was created.
};

/// @brief The three ways a present database cannot be read.
export enum class reader_error_kind : std::uint8_t {
  busy,         ///< Still locked by another connection after the 2 second bound.
  unreadable,   ///< Could not be opened or read: permissions, I/O, or not a database.
  incompatible, ///< Missing the build queue identity row or a column the reader needs.
};

/// @brief A failed read: the kind, and one line naming the cause for the operator.
/// Consumers print it as `the build queue database cannot be read: <reason>`.
export struct reader_error {
  reader_error_kind kind = reader_error_kind::unreadable; ///< Which failure this is.
  std::string       reason;                               ///< Names the file, table or column at fault.
};

/// @brief One ended run that carries the requested tag.
export struct tagged_run {
  std::vector<std::string>    argv;         ///< The command's arguments, decoded from the stored JSON array.
  std::string                 cwd;          ///< The directory the command ran in.
  std::string                 outcome;      ///< The stored outcome text, such as `exited` or `timeout`.
  std::optional<std::int64_t> exit_code;    ///< The exit status, or nullopt when none was recorded.
  std::int64_t                ended_at = 0; ///< Wall clock at the end, milliseconds since the epoch.
};

/// @brief The database header: its state, creation time and tagged retention, without runs.
export struct queue_facts {
  reader_state               state = reader_state::absent; ///< Whether the file exists.
  std::optional<std::string> created_at; ///< `queue_identity.created_at` as stored (ISO-8601 text); nullopt when absent.
  int                        tagged_history_days = 90; ///< Retention for tagged runs; 90 when absent, unset or invalid.
};

/// @brief The header of `queue_facts` plus the runs carrying one tag, in
/// `ended_at`, then sequence, order.
export struct tagged_runs {
  reader_state               state = reader_state::absent; ///< Whether the file exists.
  std::optional<std::string> created_at;                   ///< `queue_identity.created_at` as stored; nullopt when absent.
  int                        tagged_history_days = 90;     ///< Retention for tagged runs; 90 when absent, unset or invalid.
  std::vector<tagged_run>    runs;                         ///< Empty when absent.
};

/// @brief Opens an existing build queue database for reading only.
///
/// Never creates the file. Sets a 2 second busy bound and `pragma query_only =
/// on` before returning, so every statement on the returned connection that
/// would write is refused by SQLite.
/// @param path The database file. It must already exist.
/// @return The read-only connection, `busy` when locked past the bound, or
/// `unreadable` when the file cannot be opened.
export auto open_reader(const std::filesystem::path& path) -> std::expected<planar::db::connection, reader_error>;

/// @brief Reads the database header and the runs tagged `tag`, read-only.
/// @param path The build queue database file.
/// @param tag The caller's tag; only history rows with exactly this tag are returned.
/// @return The header and runs, `absent` for a missing file, or a `reader_error`.
export auto read_tagged_runs(const std::filesystem::path& path, std::string_view tag) -> std::expected<tagged_runs, reader_error>;

/// @brief Reads the database header only, read-only. It reads no history rows,
/// so it needs only the identity row and the settings table.
/// @param path The build queue database file.
/// @return The header, `absent` for a missing file, or a `reader_error`.
export auto read_queue_facts(const std::filesystem::path& path) -> std::expected<queue_facts, reader_error>;

} // namespace planar::buildqueue
