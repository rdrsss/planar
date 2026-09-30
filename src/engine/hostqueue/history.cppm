/// @file history.cppm
/// @brief `planar.engine.hostqueue.history` — how a queue entry ends: the
/// transaction that removes an entry and writes its one `queue_history` row,
/// the canceller encoding, successor recording, the history reads, and the
/// retention prune that `enqueue` runs (plan 1080, task hq-history).
///
/// Entry points:
/// - `end_entry` deletes an entry and inserts its history row in one
///   transaction (tech spec 647 § Finishing and history). Only the process
///   whose delete removed the entry writes the row; a caller that finds the
///   entry already gone writes nothing and gets `end_result::already_gone`,
///   a value rather than an error, so it can tell "I ended it" from "another
///   process did" (§ Waiting and claiming a turn).
/// - `record_successor` names the entry an abandoned waiter re-enqueued as,
///   on that waiter's existing history row.
/// - `rejoin` is what a waiting submitter does when its entry is gone and its
///   history row says `abandoned`: insert a new entry and record it as the old
///   row's successor, in one transaction.
/// - `find_history` and `list_history` read rows back.
/// - `delete_expired_history` and `remove_log_files` are the two halves of
///   the retention prune (decision 1199). `enqueue` with a retention runs the
///   first inside its insert transaction and the second after the commit;
///   they are exported so that pairing is testable.
///
/// Like `planar.engine.hostqueue.queue`, this module takes an open agent
/// database connection and plain values: the wall clock (`ended_at`, the
/// prune's "now") and the retention in days arrive as arguments, never from
/// the process clock or `engine_config`. It imports only `src/lib/` modules
/// and this bucket's own queue module.
///
/// Every failure is a `queue_error`; nothing throws across the module
/// boundary. A request whose outcome fields do not fit the outcome is an
/// `invalid_request` error and changes nothing.

module;

export module planar.engine.hostqueue.history;

import std;
import planar.db;
import planar.engine.hostqueue.queue;

namespace planar::engine::hostqueue {

/// @brief How an entry ended, mirroring the `queue_history.outcome` CHECK and
/// the outcome table of tech spec 647 § Finishing and history.
export enum class history_outcome : std::uint8_t {
  exited,       ///< The command ran and exited; the exit code is recorded.
  signaled,     ///< The command was terminated by a signal; the signal is recorded.
  timeout,      ///< The command was stopped at its run limit.
  cancelled,    ///< The entry was cancelled; the canceller is recorded.
  wait_timeout, ///< The wait limit was reached before the entry's turn.
  not_started,  ///< The command could not be started at its turn; the exit code is 126 or 127.
  abandoned,    ///< The entry was reaped because it was no longer live.
};

/// @brief The text `queue_history.outcome` stores for `outcome`.
/// @param outcome The outcome to name.
/// @return One of the seven CHECK values.
export auto to_string(history_outcome outcome) -> std::string_view;

/// @brief Parses a stored outcome.
/// @param text The column text.
/// @return The outcome, or `std::nullopt` for text outside the CHECK set.
export auto parse_history_outcome(std::string_view text) -> std::optional<history_outcome>;

/// @brief Who cancelled an entry (decision 1193). Stored in the
/// `cancelled_by` columns as a JSON object with the members `vendor`, `role`
/// and `pid`; an unknown vendor or role is JSON `null`.
export struct canceller {
  std::optional<std::string> vendor;  ///< The cancelling agent's vendor, when known.
  std::optional<std::string> role;    ///< The cancelling agent's role, when known.
  std::int64_t               pid = 0; ///< The cancelling process id.

  /// @brief Member-wise equality.
  /// @param other The canceller to compare with.
  /// @return Whether vendor, role and pid are all equal.
  auto operator==(const canceller& other) const -> bool = default;
};

/// @brief Encodes a canceller as the JSON object the `cancelled_by` columns
/// store, members in the order `vendor`, `role`, `pid`.
/// @param who The canceller.
/// @return The JSON text.
export auto encode_canceller(const canceller& who) -> std::string;

/// @brief Decodes a `cancelled_by` column.
/// @param text The JSON text the column holds.
/// @return The canceller, or `malformed_canceller` when `text` is not an
/// object whose `pid` is an integer and whose `vendor` and `role` are each a
/// string or `null`.
export auto decode_canceller(std::string_view text) -> std::expected<canceller, queue_error>;

/// @brief How the caller observed an entry end. The fields beyond `outcome`
/// and `ended_at` must fit the outcome:
/// - `exited` requires `exit_code`; `not_started` requires `exit_code` 126 or
///   127; every other outcome requires it absent.
/// - `signaled` requires `signal`; every other outcome requires it absent.
/// - `cancelled` requires a canceller, from `cancelled_by` or, when that is
///   absent, from the entry's own `cancelled_by` column (set when the cancel
///   marked the entry terminating); every other outcome requires
///   `cancelled_by` absent.
export struct end_request {
  history_outcome             outcome = history_outcome::exited; ///< How the entry ended.
  std::optional<std::int64_t> exit_code;                         ///< The exit code, for `exited` and `not_started`.
  std::optional<std::int64_t> signal;                            ///< The terminating signal, for `signaled`.
  std::optional<canceller>    cancelled_by;                      ///< The canceller, for `cancelled`.
  std::int64_t                ended_at = 0;                      ///< Wall clock at the end, ms since the epoch; display only.
  std::optional<std::int64_t> successor_seq;                     ///< The entry that replaced this one, when known already.
};

/// @brief What `end_entry` did.
export enum class end_result : std::uint8_t {
  ended,        ///< This call deleted the entry and wrote its history row.
  already_gone, ///< No entry had that number; nothing was written.
};

/// @brief One `queue_history` row, column for column.
export struct history_row {
  std::int64_t                seq     = 0;                       ///< The ended entry's sequence number.
  history_outcome             outcome = history_outcome::exited; ///< How it ended.
  std::optional<std::int64_t> exit_code;                         ///< The exit code, when recorded.
  std::optional<std::int64_t> signal;                            ///< The signal, when recorded.
  std::optional<std::int64_t> successor_seq;                     ///< The entry that replaced it, when any.
  std::optional<canceller>    cancelled_by;                      ///< Who cancelled it, when cancelled.
  bool                        nested = false;                    ///< Whether it was a nested run.
  std::optional<std::int64_t> parent_seq;                        ///< The enclosing entry of a nested run.
  std::string                 cwd;                               ///< The directory the command ran in.
  std::vector<std::string>    argv;                              ///< The command and its arguments.
  std::optional<std::string>  label;                             ///< The operator-facing label.
  std::optional<std::string>  vendor;                            ///< The submitting agent's vendor.
  std::optional<std::string>  role;                              ///< The submitting agent's role.
  std::optional<std::string>  log_path;                          ///< The output file of a detached run.
  std::int64_t                enqueued_at = 0;                   ///< Wall clock at submission, ms.
  std::optional<std::int64_t> started_at;                        ///< Wall clock at start, ms, when it started.
  std::int64_t                ended_at  = 0;                     ///< Wall clock at the end, ms.
  std::int64_t                waited_ms = 0; ///< Time from submission to start (or to the end, when it never started).
  std::optional<std::int64_t> ran_ms;        ///< Time from start to the end, when it started.
};

/// @brief Ends an entry: deletes it from `queue_entries` and inserts its
/// `queue_history` row in one write transaction (`BEGIN IMMEDIATE`, or a
/// savepoint when `conn` is already in a transaction). The row copies the
/// entry's command columns, sets `nested` and `parent_seq` from the entry's
/// `parent_seq`, and computes `waited_ms` as start (or `ended_at` when the
/// entry never started) minus `enqueued_at` and `ran_ms` as `ended_at` minus
/// start, each clamped at zero because the wall clock can step backwards.
/// @param conn An open agent database at or above agent schema version 2.
/// @param seq The entry to end.
/// @param request How it ended.
/// @return `ended` when this call removed the entry and wrote the row;
/// `already_gone` when there was no such entry, in which case nothing was
/// written; `invalid_request` when the request's fields do not fit its
/// outcome (the entry is left in place); or the SQLite failure, after which
/// the transaction is rolled back and the entry is left in place.
export auto end_entry(db::connection& conn, std::int64_t seq, const end_request& request)
    -> std::expected<end_result, queue_error>;

/// @brief What `record_successor` did.
export enum class successor_result : std::uint8_t {
  recorded,      ///< The history row now names the successor.
  no_such_entry, ///< No history row has that sequence number; nothing was written.
};

/// @brief Records `successor_seq` on the history row of `seq`, replacing any
/// earlier successor. Used when a reaped waiter re-enqueues (tech spec 647 §
/// Waiting and claiming a turn, outcome `abandoned`).
/// @param conn An open agent database.
/// @param seq The ended entry.
/// @param successor_seq The entry that replaced it.
/// @return Whether a row was updated, or the SQLite failure.
export auto record_successor(db::connection& conn, std::int64_t seq, std::int64_t successor_seq)
    -> std::expected<successor_result, queue_error>;

/// @brief What `rejoin` found and did.
export enum class rejoin_status : std::uint8_t {
  rejoined,      ///< The old row was `abandoned`; a new entry was inserted and named as its successor.
  no_history,    ///< No history row has that sequence number (never written, or pruned); nothing was written.
  not_abandoned, ///< The old row ended another way; nothing was written and `outcome` says how.
};

/// @brief The result of `rejoin`.
export struct rejoin_result {
  rejoin_status                  status = rejoin_status::no_history; ///< What was found and done.
  std::int64_t                   seq    = 0;                         ///< The new entry's sequence number, when `rejoined`.
  std::optional<history_outcome> outcome;                            ///< The old row's outcome, when `not_abandoned`.
};

/// @brief Puts a reaped waiter back at the back of the queue (tech spec 647 §
/// Waiting and claiming a turn): in one write transaction (`BEGIN IMMEDIATE`)
/// it reads the history row of `old_seq`, and only when the outcome is
/// `abandoned` inserts `request` as a new entry (a higher sequence number, so
/// behind every entry that arrived meanwhile) and records the new number as
/// the old row's successor. Either both writes happen or neither does. Runs
/// no retention prune: the submitter's own enqueue already did.
/// @param conn An open agent database at or above agent schema version 2.
/// @param old_seq The sequence number the submitter's missing entry had.
/// @param request The new entry, as `enqueue` takes it.
/// @return The status (see `rejoin_status`), or the failure, after which
/// nothing has been written.
export auto rejoin(db::connection& conn, std::int64_t old_seq, const enqueue_request& request)
    -> std::expected<rejoin_result, queue_error>;

/// @brief Reads one history row by sequence number.
/// @param conn An open agent database.
/// @param seq The ended entry's sequence number.
/// @return The row, `std::nullopt` when there is none (never ended, or
/// pruned), or the failure.
export auto find_history(db::connection& conn, std::int64_t seq) -> std::expected<std::optional<history_row>, queue_error>;

/// @brief Reads history rows ordered by `ended_at`, then by sequence number.
/// @param conn An open agent database.
/// @param ended_since When set, only rows whose `ended_at` is at or after
/// this wall-clock ms value.
/// @return The rows, oldest first; empty when there are none.
export auto list_history(db::connection& conn, std::optional<std::int64_t> ended_since = std::nullopt)
    -> std::expected<std::vector<history_row>, queue_error>;

/// @brief The milliseconds in one retention day.
export constexpr std::int64_t k_ms_per_day = 86'400'000;

/// @brief What `delete_expired_history` removed.
export struct expired_history {
  std::int64_t             rows_deleted = 0; ///< How many history rows were deleted.
  std::vector<std::string> log_paths;        ///< The non-null `log_path` of every deleted row.
};

/// @brief Deletes every history row whose `ended_at` is strictly before
/// `now - retention_days` days: a row exactly at the boundary is kept. Opens
/// no transaction of its own, so it joins the caller's (as `enqueue` does);
/// outside one it is a single autocommit statement. Log files are not
/// touched: pass the returned paths to `remove_log_files` after the
/// enclosing transaction commits, so a rollback never leaves a kept row
/// without its log.
/// @param conn An open agent database.
/// @param retention_days How many days rows are kept; zero keeps nothing
/// older than `now`. Must not be negative.
/// @param now The current wall clock, ms since the epoch.
/// @return The deleted count and log paths; `invalid_request` for a negative
/// retention; or the SQLite failure.
export auto delete_expired_history(db::connection& conn, std::int64_t retention_days, std::int64_t now)
    -> std::expected<expired_history, queue_error>;

/// @brief Removes each file in `paths`. A path that no longer exists is
/// already removed and is not a failure; any other filesystem error is
/// returned, and the remaining paths are still attempted.
/// @param paths The log files of pruned history rows.
/// @return One entry per path that could not be removed; empty when all were.
export auto remove_log_files(std::span<std::string const> paths) -> std::vector<log_removal_failure>;

} // namespace planar::engine::hostqueue
