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
/// Only enqueue and the read side live here as of task hq-enqueue. The poll
/// transaction, liveness, reaping, termination, history rows and nested-run
/// semantics are later tasks of the same milestone; a `parent_seq` on the
/// request is stored, and such an entry is inserted in state `running`
/// (roadmap M1: "outside the slot count and arrival order"), but nothing
/// here interprets it.
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
  query_failed,   ///< A SQLite statement failed to prepare, bind or step; `sqlite_code` and `message` say why.
  malformed_argv, ///< A stored `argv` column is not a JSON array of strings (a store written by something else).
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

/// @brief Inserts one entry and returns its sequence number. The entry is in
/// state `waiting`, or `running` when `request.parent_seq` is set (a nested
/// run never waits for a slot). The number is higher than that of every entry
/// ever inserted into this store, deleted or not.
/// @param conn An open agent database at or above agent schema version 2.
/// @param request What to record.
/// @return The assigned sequence number, or the SQLite failure.
export auto enqueue(db::connection& conn, const enqueue_request& request) -> std::expected<std::int64_t, queue_error>;

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
