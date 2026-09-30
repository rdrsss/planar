/// @file queue.cppm
/// @brief `planar.engine.hostqueue.queue` — the host-wide build and test
/// queue's store: enqueue, and the read side (`find`, `list`) over the
/// `queue_entries` table in the agent database (plan 1080, decision 1181,
/// task hq-enqueue).
///
/// The queue has no server (decision 1183): the process that submits a
/// command is the process that runs it, and its entry in `queue_entries` is
/// the whole record of that. `enqueue` inserts the entry and returns the
/// sequence number the database assigned; the store's `autoincrement` key
/// guarantees a later entry always gets a higher number than every earlier
/// one, including entries that have since been deleted, so arrival order
/// (decision 1178) is the sequence order.
///
/// This module takes an already-opened agent database connection (see
/// `planar.db.agentdb`) and plain values for everything the caller knows:
/// host identity, process id and start time, the clocks. It reads neither
/// the process nor the environment, and it imports only `src/lib/` modules
/// (tech spec 647 § Components: an engine target never depends on another
/// engine target, so configuration and process identity arrive as
/// arguments).
///
/// Enqueue and the read side live here; how an entry ends, its history row
/// and the retention prune live in `planar.engine.hostqueue.history` (task
/// hq-history). The enqueue that takes a retention prunes expired history in
/// the transaction that inserts the entry (tech spec 647 § Finishing and
/// history: "History rows older than the retention period are deleted at
/// enqueue"). The poll transaction, liveness, reaping, termination and
/// nested-run semantics are later tasks of the same milestone; a
/// `parent_seq` on the request is stored, and such an entry is inserted in
/// state `running` (roadmap M1: "outside the slot count and arrival order"),
/// but nothing here interprets it.
///
/// Every failure is a `queue_error` carrying the SQLite result code and the
/// driver's message; nothing throws across the module boundary.

module;

export module planar.engine.hostqueue.queue;

import std;
import planar.db;

namespace planar::engine::hostqueue {

/// @brief What went wrong in a queue operation.
export enum class queue_error_kind : std::uint8_t {
  query_failed,        ///< A SQLite statement failed to prepare, bind or step; `sqlite_code` and `message` say why.
  malformed_argv,      ///< A stored `argv` column is not a JSON array of strings (a store written by something else).
  malformed_canceller, ///< A stored `cancelled_by` column is not the canceller object `encode_canceller` writes.
  unknown_state,   ///< A stored `state` is neither `waiting` nor `running` (a store written by a newer binary or something else).
  invalid_request, ///< The caller's arguments do not fit the operation (see the entry point's contract); nothing was written.
  clock_failed,    ///< The monotonic clock could not be read; nothing was written.
};

/// @brief The failure every fallible entry point reports.
export struct queue_error {
  queue_error_kind kind        = queue_error_kind::query_failed; ///< Which failure this is.
  int              sqlite_code = 0;                              ///< The SQLite extended result code, when SQLite failed.
  std::string      message;                                      ///< A complete, printable description.
};

/// @brief An entry's state, mirroring the `queue_entries.state` CHECK.
export enum class entry_state : std::uint8_t {
  waiting, ///< Waiting for its turn.
  running, ///< Its command has started (or it is nested inside a running entry).
};

/// @brief The text `queue_entries.state` stores for `state`.
/// @param state The state to name.
/// @return `waiting` or `running`.
export auto to_string(entry_state state) -> std::string_view;

/// @brief What a submitter records when it enqueues a command. Every value is
/// supplied by the caller: the engine reads nothing from the process.
export struct enqueue_request {
  std::string                 host_id;            ///< The submitter's host identity (the literal `unknown` when unreadable).
  std::int64_t                pid         = 0;    ///< The submitter's process id.
  std::int64_t                pid_started = 0;    ///< The submitter's process start time, as the platform reports it.
  std::string                 cwd;                ///< The directory the command runs in.
  std::vector<std::string>    argv;               ///< The command and its arguments, stored losslessly.
  std::optional<std::string>  label;              ///< An operator-facing label, when given.
  std::optional<std::string>  vendor;             ///< The submitting agent's vendor, when known.
  std::optional<std::string>  role;               ///< The submitting agent's role, when known.
  std::optional<std::string>  claim_token;        ///< The claim the submitter works under, when any.
  std::optional<std::string>  log_path;           ///< The output file of a detached run, when any.
  std::int64_t                enqueued_at    = 0; ///< Wall clock at submission, ms since the epoch; display only.
  std::int64_t                refreshed_mono = 0; ///< Monotonic clock at submission, ms; the freshness baseline.
  std::optional<std::int64_t> wait_deadline_mono; ///< Monotonic ms after which waiting gives up, when limited.
  std::optional<std::int64_t> parent_seq;         ///< The enclosing running entry, for a nested run.
};

/// @brief One `queue_entries` row, column for column.
export struct entry {
  std::int64_t                seq   = 0;                    ///< The sequence number, assigned by the store.
  entry_state                 state = entry_state::waiting; ///< `waiting` or `running`.
  std::string                 host_id;                      ///< The submitter's host identity.
  std::int64_t                pid         = 0;              ///< The submitter's process id.
  std::int64_t                pid_started = 0;              ///< The submitter's process start time.
  std::optional<std::int64_t> child_pgid;                   ///< The command's process group, once started.
  std::optional<std::int64_t> child_started;                ///< The start time of the child group's leader.
  std::optional<std::int64_t> parent_seq;                   ///< The enclosing entry of a nested run.
  std::optional<std::int64_t> terminating_since_mono;       ///< Monotonic ms at which stopping began.
  std::optional<std::string>  terminate_reason;             ///< `timeout` or `cancelled`, while stopping.
  std::optional<std::string>  cancelled_by;                 ///< Who cancelled it, when cancelled.
  std::string                 cwd;                          ///< The directory the command runs in.
  std::vector<std::string>    argv;                         ///< The command and its arguments.
  std::optional<std::string>  label;                        ///< The operator-facing label.
  std::optional<std::string>  vendor;                       ///< The submitting agent's vendor.
  std::optional<std::string>  role;                         ///< The submitting agent's role.
  std::optional<std::string>  claim_token;                  ///< The submitter's claim.
  std::optional<std::string>  log_path;                     ///< The output file of a detached run.
  std::int64_t                enqueued_at = 0;              ///< Wall clock at submission, ms.
  std::optional<std::int64_t> started_at;                   ///< Wall clock at start, ms.
  std::int64_t                refreshed_mono = 0;           ///< Monotonic ms of the last refresh.
  std::optional<std::int64_t> deadline_mono;                ///< Monotonic ms after which a running command is stopped.
  std::optional<std::int64_t> wait_deadline_mono;           ///< Monotonic ms after which waiting gives up.
};

/// @brief Encodes an argument vector as the JSON array of strings the
/// `argv` column stores. Every byte of every argument survives: spaces,
/// quotes, newlines and non-ASCII text are escaped by the JSON grammar, not
/// by any shell convention.
/// @param argv The arguments.
/// @return The JSON text.
export auto encode_argv(std::span<std::string const> argv) -> std::string;

/// @brief Decodes an `argv` column back into the argument vector.
/// @param text The JSON text the column holds.
/// @return The arguments, or `malformed_argv` when `text` is not a JSON
/// array whose every element is a string.
export auto decode_argv(std::string_view text) -> std::expected<std::vector<std::string>, queue_error>;

/// @brief Inserts one entry and returns its sequence number, without pruning
/// history (the three-argument form prunes). The entry is in
/// state `waiting`, or `running` when `request.parent_seq` is set (a nested
/// run never waits for a slot). The number is higher than that of every entry
/// ever inserted into this store, deleted or not.
/// @param conn An open agent database at or above agent schema version 2.
/// @param request What to record.
/// @return The assigned sequence number, or the SQLite failure.
export auto enqueue(db::connection& conn, const enqueue_request& request) -> std::expected<std::int64_t, queue_error>;

/// @brief A pruned history row's log file that could not be removed.
export struct log_removal_failure {
  std::string path;    ///< The log file.
  std::string message; ///< Why it could not be removed.
};

/// @brief What the retention prune at enqueue did.
export struct prune_report {
  std::int64_t                     rows_deleted = 0; ///< How many expired history rows were deleted.
  std::vector<log_removal_failure> log_failures;     ///< Log files of those rows that could not be removed.
};

/// @brief The result of an enqueue that prunes history.
export struct enqueued {
  std::int64_t seq = 0; ///< The assigned sequence number.
  prune_report pruned;  ///< What the prune removed, and any log file it could not.
};

/// @brief Inserts one entry, as the two-argument `enqueue` does, after
/// deleting every history row that ended more than `history_days` days
/// before `request.enqueued_at` (decision 1199); the delete and the insert
/// are one write transaction. The expired rows' log files are removed after
/// the commit. A log file that is already gone counts as removed; one that
/// cannot be removed is listed in `pruned.log_failures` and does not fail
/// the enqueue, because the entry is already committed and the caller's
/// command must still run. The queue verbs call this form with the
/// configured `[queue] history_days`.
/// @param conn An open agent database at or above agent schema version 2.
/// @param request What to record; `enqueued_at` is the prune's "now".
/// @param history_days The retention in days; must not be negative.
/// @return The sequence number and the prune report; `invalid_request` for a
/// negative retention; or the SQLite failure, after which neither the prune
/// nor the insert has happened.
export auto enqueue(db::connection& conn, const enqueue_request& request, std::int64_t history_days)
    -> std::expected<enqueued, queue_error>;

/// @brief Records the child group of a running entry: the group id and the
/// start time of its leader (`child_pgid`, `child_started`), which the
/// liveness rules use to keep the entry live while the group has members and
/// to detect a reused id. Written by the submitter once its command has
/// started, and only on an entry that is `running`.
/// @param conn An open agent database at or above agent schema version 2.
/// @param seq The submitter's entry.
/// @param child_pgid The command's process group id.
/// @param child_started The start time of the group's leader, as the platform
/// reports it.
/// @return `true` when the entry exists, is running, and now records the
/// group; `false` when there is no such running entry (it was reaped, or it
/// never started) and nothing was written; or the SQLite failure.
export auto record_child(db::connection& conn, std::int64_t seq, std::int64_t child_pgid, std::int64_t child_started)
    -> std::expected<bool, queue_error>;

/// @brief Records the output file of a detached run on its entry (tech spec
/// 647 § Submitting, With `--detach`): the path is `queue-logs/<seq>.log`, so
/// it is only known once the store has assigned the sequence number.
/// @param conn An open agent database at or above agent schema version 2.
/// @param seq The submitter's entry.
/// @param log_path The output file's path.
/// @return `true` when the entry exists and now records the path; `false`
/// when there is no such entry and nothing was written; or the SQLite failure.
export auto set_log_path(db::connection& conn, std::int64_t seq, std::string_view log_path) -> std::expected<bool, queue_error>;

/// @brief Takes an entry back out of the queue WITHOUT writing a history row.
/// It is for an entry that was inserted and never became a run, so no history
/// is owed: a detached submitter that cannot create its log file, or cannot
/// hand its ticket over, removes the entry it inserted with this (tech spec
/// 647 § Submitting, With `--detach`, step 4). An entry that has run is ended
/// with `end_entry`, which writes the one history row.
/// @param conn An open agent database.
/// @param seq The entry to remove.
/// @return `true` when an entry was removed; `false` when there was none; or
/// the SQLite failure.
export auto discard_entry(db::connection& conn, std::int64_t seq) -> std::expected<bool, queue_error>;

/// @brief Reads one entry by sequence number.
/// @param conn An open agent database.
/// @param seq The sequence number.
/// @return The entry, `std::nullopt` when no entry has that number (it never
/// existed, or it has ended and been removed), or the failure.
export auto find(db::connection& conn, std::int64_t seq) -> std::expected<std::optional<entry>, queue_error>;

/// @brief Reads every entry, in sequence order.
/// @param conn An open agent database.
/// @return The entries, lowest sequence number first; empty when the queue
/// is empty.
export auto list(db::connection& conn) -> std::expected<std::vector<entry>, queue_error>;

} // namespace planar::engine::hostqueue
