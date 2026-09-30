/// @file nested.cppm
/// @brief `planar.engine.hostqueue.nested` — the entry of a nested run: a
/// `queue run` started by a queued command (plan 1080, task hq-nested-entry;
/// tech spec 647 § Nested runs, decision 1191).
///
/// Entry point: `enqueue_nested(conn, parent_seq, request, limits, clock,
/// probe)`. In one `BEGIN IMMEDIATE` transaction it reads the named parent
/// entry and, only when that parent exists, is `running` and passes the
/// liveness rules of `planar.engine.hostqueue.liveness`, inserts a new entry
/// that:
///
/// - records `parent_seq`, so the poll leaves it out of the slot count and
///   arrival order, and ending it writes a history row marked `nested` that
///   names the parent (`end_entry`);
/// - is in state `running` from the start, with `started_at` from the wall
///   clock, `deadline_mono` = now + the run limit and `run_limit_ms` = the run
///   limit, like any entry whose turn has come;
/// - is refreshed at now.
///
/// A parent may itself be a nested entry, so a nested run can contain
/// another.
///
/// When the parent is missing, waiting, not live, or its liveness cannot be
/// judged because a process query failed, nothing is inserted and the
/// result is `nested_status::queue_normally` with the reason: decision 1191
/// honours the slot marker only when it names a live running entry, and the
/// caller then enqueues the command as an ordinary waiting entry. A failed
/// liveness query refuses rather than accepts, so an unjudged marker can
/// delay a command but never start one outside the queue.
///
/// The monotonic clock is read once, after the write lock is held (decision
/// 1203), and is the "now" of both the parent's liveness judgement and the
/// new entry's deadline. The wall clock is read for `started_at` only.
///
/// Reading the `PLANAR_QUEUE_SLOT` marker from the environment is the
/// `queue run` verb's (task hq-nested-run); this module takes the parent's
/// sequence number as a value. Error boundary: every failure is a
/// `queue_error`, after which the transaction is rolled back and nothing has
/// changed; nothing throws across the module boundary. The module imports
/// only `src/lib/` modules and this bucket's own modules.

module;

export module planar.engine.hostqueue.nested;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

/// @brief The windows a nested insert applies, from the caller's `[queue]`
/// configuration.
export struct nested_limits {
  std::int64_t stale_after_ms = 0; ///< The staleness window the parent's liveness is judged with, ms; must not be negative.
  std::int64_t run_limit_ms   = 0; ///< The run limit the nested entry's deadline is set from, ms; must not be negative.
};

/// @brief Whether a nested entry was inserted.
export enum class nested_status : std::uint8_t {
  inserted,       ///< The parent is a live running entry; the nested entry was inserted `running`.
  queue_normally, ///< The parent does not qualify; nothing was inserted, and the caller queues the command normally.
};

/// @brief Why a parent does not qualify.
export enum class nested_refusal : std::uint8_t {
  parent_missing,    ///< No entry has the parent's sequence number (it never existed, or it has ended).
  parent_waiting,    ///< The parent is still waiting for its turn.
  parent_not_live,   ///< The parent is running but fails the liveness rules.
  parent_unjudged,   ///< A process query failed, so the parent's liveness could not be judged.
  parent_other_host, ///< The parent is on another host identity, or on an unknown one (decision 1209).
};

/// @brief What `enqueue_nested` did.
export struct nested_result {
  nested_status                 status = nested_status::queue_normally; ///< Whether the entry was inserted.
  std::optional<nested_refusal> refusal;                                ///< Why not, when `status` is `queue_normally`.
  std::int64_t                  seq      = 0;                           ///< The nested entry's sequence number, when inserted.
  std::int64_t                  now_mono = 0;                           ///< The monotonic time this call compared against.
  std::optional<std::int64_t>   started_at;                             ///< The nested entry's wall-clock start, when inserted.
  std::optional<std::int64_t>   deadline_mono;                          ///< The nested entry's deadline, when inserted.
};

/// @brief Inserts a nested entry under `parent_seq`, as this module's
/// description states, or refuses and inserts nothing.
/// @param conn An open agent database at or above agent schema version 3,
/// not inside a transaction.
/// @param parent_seq The entry named by the slot marker.
/// @param request What to record. `request.host_id` is also the checker's
/// host identity for the parent's liveness. `request.parent_seq` must be
/// unset or equal to `parent_seq`; `request.refreshed_mono` is replaced by
/// the monotonic time read under the write lock.
/// @param limits The staleness window and run limit.
/// @param clock The monotonic and wall clocks; the monotonic clock is read
/// once, after the write lock is held.
/// @param probe The process queries the parent's liveness uses.
/// @return `inserted` with the sequence number, start time and deadline;
/// `queue_normally` with the refusal; `invalid_request` for a negative
/// window or run limit, a conflicting `request.parent_seq`, or a connection
/// already in a transaction; `clock_failed` when the monotonic clock could
/// not be read; or the SQLite failure. After any error nothing has changed.
export auto enqueue_nested(db::connection& conn, std::int64_t parent_seq, const enqueue_request& request,
                           const nested_limits& limits, process::identity::clock& clock, const process_probe& probe)
    -> std::expected<nested_result, queue_error>;

} // namespace planar::engine::hostqueue
