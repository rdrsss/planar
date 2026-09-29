/// @file poll.cppm
/// @brief `planar.engine.hostqueue.poll` — the poll a submitter runs at every
/// interval: one `BEGIN IMMEDIATE` transaction that refreshes the caller's
/// entry, reaps entries that are not live, marks overdue orphans terminating,
/// and gives the caller its turn (plan 1080, task hq-poll-transaction; tech
/// spec 647 § Waiting and claiming a turn).
///
/// Entry point: `poll(conn, request, clock, probe)`, where `clock` is the
/// `planar.process.identity` clock pair, so a test drives time by hand. In one write
/// transaction, in this order, it:
///
/// 1. refreshes the caller's entry (`refreshed_mono` = now);
/// 2. ends every other entry that is not live with outcome `abandoned`,
///    through `end_entry`, so each gets exactly one history row in this same
///    transaction;
/// 3. marks as terminating (`terminating_since_mono` = now, `terminate_reason`
///    = `timeout`) every running entry that is past its deadline, is not
///    already terminating, and whose submitter is gone;
/// 4. when the caller's entry is `waiting` and is among the first `slots`
///    live entries that are not nested, by sequence number, sets it `running`
///    with `started_at` from the wall clock and `deadline_mono` = now + the run
///    limit. An entry already running is left as it is, so its start time and
///    deadline are set once.
///
/// The caller's own entry is never judged for reaping or marking: the
/// calling process is its submitter and has just refreshed it.
///
/// The monotonic clock is read once, after `BEGIN IMMEDIATE` has taken the
/// write lock (decision 1203), so no concurrent refresh can land later than
/// the time this poll compares against. The wall clock is read for display
/// columns only (`started_at`, the history rows' `ended_at`) and is never
/// compared.
///
/// No signal is sent. Entries this poll marked terminating are returned so
/// the caller can signal them after the commit (task hq-terminate). An entry
/// whose liveness could not be judged because a process query failed is
/// neither reaped nor marked; it is returned in `liveness_errors` and still
/// counts toward the turn, so a failing query can delay a start but never
/// oversubscribe the slots.
///
/// Error boundary: when `BEGIN IMMEDIATE` fails because the store is busy
/// past the connection's busy timeout, the poll changes nothing and returns
/// `poll_status::skipped` (a value, not an error); the caller retries at its
/// next interval. Every other failure is a `queue_error`, after which the
/// transaction is rolled back and nothing has changed. Nothing throws across
/// the module boundary. Like the rest of the bucket, the module takes slot
/// counts, intervals, the host identity, the clocks and the process probe as
/// arguments and imports only `src/lib/` modules and this bucket's own
/// modules.

module;

export module planar.engine.hostqueue.poll;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

/// @brief What the polling submitter supplies.
export struct poll_request {
  std::int64_t seq = 0;            ///< The caller's own entry.
  std::string  host_id;            ///< The caller's host identity (`unknown` when unreadable).
  std::int64_t slots          = 1; ///< The slot count N in force for this poll; must not be negative.
  std::int64_t stale_after_ms = 0; ///< The staleness window, ms; must not be negative.
  std::int64_t run_limit_ms   = 0; ///< The run limit, ms, from which a starting entry's deadline is set; must not be negative.
};

/// @brief Whether the poll ran.
export enum class poll_status : std::uint8_t {
  completed, ///< The transaction ran and committed.
  skipped,   ///< The store was busy past the busy timeout; nothing changed.
};

/// @brief An entry whose liveness could not be judged in this poll.
export struct liveness_failure {
  std::int64_t             seq   = 0;                                      ///< The entry.
  process::identity::error error = process::identity::error::query_failed; ///< The failed process query.
};

/// @brief What one poll did, and where the caller's entry stands.
export struct poll_result {
  poll_status  status   = poll_status::completed; ///< Whether the poll ran; every other field is empty when skipped.
  std::int64_t now_mono = 0;                      ///< The monotonic time this poll compared against.
  bool         entry_missing =
      false; ///< The caller's entry was not in the store; step 4 did not run (the caller applies the missing-entry rule).
  bool                        running = false; ///< The caller's entry is `running` after this poll.
  bool                        started = false; ///< This poll set the caller's entry `running`.
  std::optional<std::int64_t> started_at;      ///< The caller's entry's wall-clock start, when running.
  std::optional<std::int64_t> deadline_mono;   ///< The caller's entry's deadline, when running.
  std::vector<std::int64_t>   reaped;          ///< Entries this poll ended with outcome `abandoned`, in sequence order.
  std::vector<entry> terminating; ///< Entries this poll marked terminating, as marked; the caller signals them after commit.
  std::vector<liveness_failure>
      liveness_errors; ///< Entries not judged because a process query failed; neither reaped nor marked.
};

/// @brief Runs one poll for the caller's entry, as this module's description
/// states.
/// @param conn An open agent database at or above agent schema version 2,
/// not inside a transaction (the poll must take the write lock itself).
/// @param request The caller's entry, host identity, slot count, staleness
/// window and run limit.
/// @param clock The monotonic and wall clocks; the monotonic clock is read
/// once, after the write lock is held.
/// @param probe The process queries liveness uses.
/// @return The result; `status == skipped` when the store was busy past the
/// busy timeout; `invalid_request` for a negative slot count, window or run
/// limit, or a connection already in a transaction; `clock_failed` when the
/// monotonic clock could not be read; or the SQLite failure. After any
/// error nothing has changed.
export auto poll(db::connection& conn, const poll_request& request, process::identity::clock& clock, const process_probe& probe)
    -> std::expected<poll_result, queue_error>;

} // namespace planar::engine::hostqueue
