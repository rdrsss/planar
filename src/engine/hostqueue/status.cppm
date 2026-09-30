/// @file status.cppm
/// @brief `planar.engine.hostqueue.status` — what `queue status <seq>` reports
/// about one sequence number, read without changing anything (plan 1080, task
/// hq-queue-status; tech spec 647 § CLI surface).
///
/// `query_status` answers from the entry while it exists and from its history
/// row afterwards. It judges liveness by the same rules a poll applies
/// (`judge_liveness`) but only reads: it never reaps, refreshes, marks or ends
/// anything, and every statement it runs is a `select`, so it is safe on a
/// connection opened read-only.
///
/// When the number asked for has a history row whose `successor_seq` is set (a
/// reaped waiter that rejoined the queue), the answer describes the successor
/// and `superseded_by` names it; a successor that has itself been reaped and
/// has its own successor is followed to the end of the chain. `seq` stays the
/// number that was asked for. A successor with neither an entry nor a history
/// row (pruned) ends the chain at the last row that exists.
///
/// The fields mirror the `--json` field set in the tech spec. A member that
/// does not apply to the state found is empty. `run_limit_ms` and
/// `wait_limit_ms` are always empty: the store records a deadline on a
/// monotonic clock, not the limit an entry was submitted with, so the limit
/// cannot be recovered from it.
///
/// Error boundary: a store failure is a `queue_error`; the configuration is
/// asked for only when the entry described is still in the queue, and its
/// failure is a `status_error` of kind `settings`. Nothing throws.
module;

export module planar.engine.hostqueue.status;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.history;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

/// @brief Where the entry described stands.
export enum class status_state : std::uint8_t {
  waiting, ///< In the queue, waiting for its turn.
  running, ///< In the queue, its command started.
  ended,   ///< No longer in the queue; its history row says how it ended.
};

/// @brief The text the JSON `state` field carries.
/// @param state The state.
/// @return `waiting`, `running` or `ended`.
export auto to_string(status_state state) -> std::string_view;

/// @brief The configuration `query_status` needs for an entry still in the queue.
export struct status_settings {
  std::int64_t slots          = 1; ///< The slot count in force.
  std::int64_t stale_after_ms = 0; ///< The staleness window, ms, liveness is judged against.
  std::int64_t grace_ms       = 0; ///< The SIGTERM-to-SIGKILL grace period, ms.
};

/// @brief What the caller supplies to `query_status`.
export struct status_request {
  std::string  host_id;    ///< The checker's host identity (`unknown` when unreadable).
  std::int64_t now_mono{}; ///< The checker's monotonic clock, ms, liveness compares against.
  std::int64_t now_wall{}; ///< The checker's wall clock, ms; only durations of entries still in the queue are computed from it.
  /// @brief Supplies the settings; called only when the entry described is still in the queue.
  std::function<std::expected<status_settings, std::string>()> settings;
  /// @brief The process queries liveness uses.
  process_probe probe;
};

/// @brief Everything `queue status` reports about one sequence number.
export struct queue_status {
  std::int64_t                   seq   = 0;                     ///< The sequence number asked for.
  status_state                   state = status_state::waiting; ///< Where the entry described stands.
  std::optional<bool>            live;            ///< Whether it passes the liveness rules; empty when ended or not judged.
  std::optional<std::int64_t>    position;        ///< Place among waiting entries from 1, when waiting.
  std::optional<history_outcome> outcome;         ///< How it ended, when ended.
  std::optional<std::int64_t>    exit_code;       ///< The command's exit code, when recorded.
  std::optional<std::int64_t>    signal;          ///< The terminating signal, when recorded.
  std::optional<std::string>     terminating;     ///< The reason it is being stopped, when it is.
  std::optional<canceller>       cancelled_by;    ///< Who cancelled it, when cancelled.
  std::optional<std::int64_t>    superseded_by;   ///< The end of the successor chain, when re-enqueued.
  bool                           nested = false;  ///< Whether it is a nested run.
  std::optional<std::int64_t>    parent_seq;      ///< The enclosing entry of a nested run.
  std::string                    cwd;             ///< As submitted.
  std::vector<std::string>       argv;            ///< As submitted.
  std::optional<std::string>     label;           ///< As submitted.
  std::optional<std::string>     vendor;          ///< As submitted.
  std::optional<std::string>     role;            ///< As submitted.
  std::optional<std::string>     log_path;        ///< The output file of a detached run.
  std::int64_t                   enqueued_at = 0; ///< Wall clock at submission, ms.
  std::optional<std::int64_t>    started_at;      ///< Wall clock at start, ms.
  std::optional<std::int64_t>    ended_at;        ///< Wall clock at the end, ms, when ended.
  std::optional<std::int64_t>    waited_ms;       ///< Time from submission to start, or to now while waiting.
  std::optional<std::int64_t>    ran_ms;          ///< Time from start to the end, or to now while running.
  std::optional<std::int64_t>    run_limit_ms;    ///< Not recoverable from the store; always empty.
  std::optional<std::int64_t>    wait_limit_ms;   ///< Not recoverable from the store; always empty.
  std::optional<std::int64_t>    slots;           ///< The slot count in force, for an entry in the queue.
  std::optional<std::int64_t>    grace_ms;        ///< The grace period in force, for an entry in the queue.
};

/// @brief Which step of `query_status` failed.
export enum class status_error_kind : std::uint8_t {
  store,    ///< The store could not be read; `message` says why.
  settings, ///< The configuration the answer needs could not be read.
};

/// @brief The failure `query_status` reports.
export struct status_error {
  status_error_kind kind = status_error_kind::store; ///< Which step failed.
  std::string       message;                         ///< A complete, printable description.
};

/// @brief Reads what is known about `seq`, as this module's description states.
/// @param conn An open agent database; a read-only connection is enough.
/// @param seq The sequence number asked for.
/// @param request The checker's identity and clocks, the settings supplier and the process probe.
/// @return The status, `std::nullopt` when the store knows no such number (never
/// issued, or its history pruned), or the failure. Nothing is written.
export auto query_status(db::connection& conn, std::int64_t seq, const status_request& request)
    -> std::expected<std::optional<queue_status>, status_error>;

} // namespace planar::engine::hostqueue
