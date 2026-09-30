/// @file terminate.cppm
/// @brief `planar.engine.hostqueue.terminate` — stopping a running queue
/// entry's command in two steps, neither of which holds the write lock while
/// it signals or waits (plan 1080, task hq-terminate; tech spec 647 §
/// Stopping a command).
///
/// Entry points:
/// - `begin_terminate` is step one. A short `BEGIN IMMEDIATE` transaction
///   records `terminating_since_mono` (the monotonic clock, read once the
///   write lock is held), `terminate_reason` and, for a cancellation,
///   `cancelled_by` on a running entry that is not already terminating, and
///   commits. Only then, and only when this call set the marker, does it send
///   SIGTERM to the entry's child group. Calling it on an entry that is
///   already terminating changes nothing and sends nothing.
/// - `cancel_waiting` is a cancellation's other half (task hq-queue-cancel): a
///   waiting entry has no command to stop, so it is removed, with its
///   `cancelled` history row and the canceller, in one `BEGIN IMMEDIATE`
///   transaction that first checks the entry is still waiting. An entry that
///   has started is never removed by it, so a turn taken between the caller's
///   read and this call cannot strand a running command without its entry.
/// - `advance_terminations` is step two, and any process may run it. For each
///   terminating entry it examines, with no transaction open: when the child
///   group is empty, or its id has been reused (which counts as empty), it
///   ends the entry through `end_entry` with the outcome named by
///   `terminate_reason` (`timeout`, or `cancelled` with the recorded
///   canceller, or `abandoned` when a cancellation's canceller cannot be
///   read); a failed end is recorded per entry and does not stop the rest; when the group still has members and the entry has
///   been terminating for longer than the grace period, it sends SIGKILL to the group. A terminating entry whose group has
///   members is never removed, so it keeps its slot.
/// - `signal_child_group` is the one guarded path to a signal. The caller of
///   `poll` uses it to send SIGTERM to each entry the poll returned as newly
///   marked, after the poll has committed.
/// - `poll_and_stop` is the poll a submitter runs at each interval together
///   with the stopping steps that follow it (task hq-orphan-deadline): it
///   runs `poll`, and only after the poll has committed sends SIGTERM through
///   `signal_child_group` to each entry the poll marked, then runs
///   `advance_terminations`. So every polling submitter enforces an
///   orphan's deadline (decision 1184) with one call, and no signal is sent
///   while the write lock is held.
///
/// `signal_child_group` sends a signal only when every guard passes: the
/// entry's host identity equals the checker's and neither is `unknown`; the
/// entry records a child group id greater than 1 and the start time of that
/// group's leader; the id has not been reused (tech spec 647 § Liveness,
/// precisely); and the group has at least one member. So no call here signals
/// process group 0, 1 or -1, a group on another host, or a process that has
/// since taken a reused id. It takes no connection, so signalling outside any
/// transaction is the caller's obligation: `begin_terminate` and
/// `advance_terminations` enforce it, and a poll caller calls
/// `signal_child_group` only after the poll's transaction has committed.
///
/// Every signal goes through a `group_signaller`, whose default
/// (`system_group_signaller`) forwards to `planar.process.identity::
/// signal_group`; a test hands in a recorder. Process queries go through the
/// liveness module's `process_probe`, and the clock, grace period and host
/// identity arrive as arguments. The module imports only `src/lib/` modules
/// and this bucket's own modules.
///
/// Error boundary: a SQLite failure, an unreadable clock or a request that
/// does not fit the operation is a `queue_error`, after which any transaction
/// this call opened has been rolled back. A failed process query or signal is
/// not an error: it is reported on the result as a `signal_attempt` with
/// outcome `failed`, and the entry is left for a later call. Nothing throws
/// across the module boundary.

module;

export module planar.engine.hostqueue.terminate;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.history;
import planar.engine.hostqueue.liveness;
import planar.engine.hostqueue.poll;

namespace planar::engine::hostqueue {

/// @brief Why an entry is being stopped, mirroring the
/// `queue_entries.terminate_reason` CHECK.
export enum class stop_reason : std::uint8_t {
  timeout,   ///< Its run limit passed.
  cancelled, ///< It was cancelled; the canceller is recorded.
};

/// @brief The text `queue_entries.terminate_reason` stores for `reason`.
/// @param reason The reason to name.
/// @return `timeout` or `cancelled`.
export auto to_string(stop_reason reason) -> std::string_view;

/// @brief Which of the two stopping signals a call sends.
export enum class stop_signal : std::uint8_t {
  term, ///< SIGTERM, sent once by the process that set the terminating marker.
  kill, ///< SIGKILL, sent once the grace period has passed.
};

/// @brief The platform's number for `sig`.
/// @param sig The stopping signal.
/// @return `SIGTERM` or `SIGKILL`.
export auto signal_number(stop_signal sig) -> int;

/// @brief Sends a signal to every member of a process group. Has the
/// signature and meaning of `planar.process.identity::signal_group`.
export using group_signaller = std::function<std::expected<void, process::identity::error>(std::int64_t pgid, int sig)>;

/// @brief The signaller that signals the host's process groups.
/// @return A signaller forwarding to `planar.process.identity::signal_group`.
export auto system_group_signaller() -> group_signaller;

/// @brief Whether a guarded signal was sent, and if not, why.
export enum class signal_outcome : std::uint8_t {
  sent,        ///< The signaller delivered the signal to the group.
  other_host,  ///< The entry's host identity is not the checker's (or one is `unknown`); its ids are never used here.
  no_group,    ///< The entry records no child group id above 1, or no leader start time; nothing can be verified.
  group_empty, ///< The group has no member.
  reused,      ///< A process with the group's id has a different start time; the id belongs to someone else.
  failed,      ///< A process query or the signal itself failed; `error` says which.
};

/// @brief One guarded signal and what came of it.
export struct signal_attempt {
  std::int64_t                            seq     = 0;                        ///< The entry whose group was the target.
  stop_signal                             signal  = stop_signal::term;        ///< Which stopping signal.
  signal_outcome                          outcome = signal_outcome::no_group; ///< Whether it was sent.
  std::optional<process::identity::error> error;                              ///< The failure, when `outcome` is `failed`.
};

/// @brief Sends `sig` to `e`'s child group when every guard in this module's
/// description passes. Never call it with a transaction open on the store:
/// signals are sent only after the marker they act on has committed.
/// @param e The entry, as read from the store.
/// @param sig Which stopping signal.
/// @param host_id The checker's host identity (`unknown` when unreadable).
/// @param probe The process queries.
/// @param signaller Where the signal goes.
/// @return What was done.
export auto signal_child_group(const entry& e, stop_signal sig, std::string_view host_id, const process_probe& probe,
                               const group_signaller& signaller) -> signal_attempt;

/// @brief What the stopping process supplies to `begin_terminate`.
export struct begin_terminate_request {
  std::int64_t             seq    = 0;                    ///< The entry to stop.
  stop_reason              reason = stop_reason::timeout; ///< Why it is stopped.
  std::optional<canceller> cancelled_by;                  ///< The canceller; required for `cancelled`, absent for `timeout`.
  std::string              host_id;                       ///< The checker's host identity (`unknown` when unreadable).
};

/// @brief What `begin_terminate` found and did.
export enum class begin_status : std::uint8_t {
  marked,              ///< This call set the marker and committed it; `sigterm` says whether SIGTERM was sent.
  already_terminating, ///< The entry already carried a marker; nothing was written and no signal was sent.
  not_running,         ///< The entry is waiting, so there is no command to stop; nothing was written.
  missing,             ///< No entry has that number; nothing was written.
};

/// @brief The result of `begin_terminate`.
export struct begin_result {
  begin_status                  status = begin_status::missing; ///< What was found and done.
  std::optional<entry>          stored;                         ///< The entry as it stands after the transaction, when it exists.
  std::optional<signal_attempt> sigterm;                        ///< The SIGTERM attempt, set only when `status` is `marked`.
};

/// @brief Step one of stopping a command, as this module's description
/// states: marks the entry in one committed transaction, then sends SIGTERM
/// to its child group.
/// @param conn An open agent database at or above agent schema version 2,
/// not inside a transaction.
/// @param request The entry, the reason, the canceller and the checker's
/// host identity.
/// @param clock The monotonic clock is read once, after the write lock is
/// held.
/// @param probe The process queries that guard the signal.
/// @param signaller Where SIGTERM goes.
/// @return The result; `invalid_request` when the connection is in a
/// transaction or the canceller does not fit the reason; `clock_failed`; or
/// the SQLite failure (a store busy past the busy timeout included), after
/// which nothing was written and no signal was sent.
export auto begin_terminate(db::connection& conn, const begin_terminate_request& request, process::identity::clock& clock,
                            const process_probe& probe, const group_signaller& signaller)
    -> std::expected<begin_result, queue_error>;

/// @brief What `cancel_waiting` found and did.
export enum class cancel_waiting_status : std::uint8_t {
  removed,     ///< The entry was waiting; this call removed it and wrote its `cancelled` history row.
  not_waiting, ///< The entry exists and is not waiting (it has started); nothing was written.
  missing,     ///< No entry has that number; nothing was written.
};

/// @brief The result of `cancel_waiting`.
export struct cancel_waiting_result {
  cancel_waiting_status status = cancel_waiting_status::missing; ///< What was found and done.
  std::optional<entry>  stored;                                  ///< The entry as it stood, for `not_waiting`.
};

/// @brief Removes a waiting entry as cancelled: one `BEGIN IMMEDIATE`
/// transaction reads the entry and, only when it is waiting, ends it through
/// `end_entry` with outcome `cancelled` and `who`.
/// @param conn An open agent database at or above agent schema version 2,
/// not inside a transaction.
/// @param seq The entry to cancel.
/// @param who The canceller, recorded on the history row.
/// @param ended_at Wall clock at the cancellation, ms since the epoch; display only.
/// @return What was found and done; `invalid_request` when the connection is
/// already in a transaction; or the SQLite failure, after which nothing was
/// written.
export auto cancel_waiting(db::connection& conn, std::int64_t seq, const canceller& who, std::int64_t ended_at)
    -> std::expected<cancel_waiting_result, queue_error>;

/// @brief What the advancing process supplies to `advance_terminations`.
export struct advance_request {
  std::string                 host_id;      ///< The checker's host identity (`unknown` when unreadable).
  std::int64_t                grace_ms = 0; ///< How long an entry may be terminating before SIGKILL; must not be negative.
  std::optional<std::int64_t> seq;          ///< When set, only this entry is examined (`queue cancel` finishing its own).
};

/// @brief An entry `advance_terminations` ended.
export struct ended_termination {
  std::int64_t    seq     = 0;                        ///< The entry.
  history_outcome outcome = history_outcome::timeout; ///< The outcome written: `timeout` or `cancelled`.
};

/// @brief A terminating entry `advance_terminations` could not end.
export struct end_failure {
  std::int64_t seq = 0; ///< The entry, left in place for a later call.
  queue_error  error;   ///< Why ending it failed.
};

/// @brief What `advance_terminations` did.
export struct advance_result {
  std::int64_t                   now_mono = 0; ///< The monotonic time the grace period was measured against.
  std::vector<signal_attempt>    kills;        ///< Every SIGKILL attempt, in sequence order, sent or not.
  std::vector<ended_termination> ended;        ///< Entries this call ended, in sequence order.
  std::vector<signal_attempt>    failures;     ///< Entries whose group could not be judged; neither ended nor signalled.
  std::vector<end_failure> end_failures; ///< Entries whose group was empty but whose end failed; the rest were still examined.
};

/// @brief Step two of stopping a command, as this module's description
/// states. Only terminating entries on the checker's host that record a
/// child group are examined; any other entry is left alone.
/// @param conn An open agent database at or above agent schema version 2,
/// not inside a transaction.
/// @param request The checker's host identity, the grace period and an
/// optional single entry.
/// @param clock The monotonic clock is read once, before any entry is
/// examined.
/// @param probe The process queries.
/// @param signaller Where SIGKILL goes.
/// @return The result; `invalid_request` for a negative grace period or a
/// connection in a transaction; `clock_failed`; or the SQLite failure of
/// listing the entries. A failure to end one entry is not an error of the
/// call: it is recorded in `end_failures`, the entry is left for a later
/// call, and the remaining entries are still examined, so the kills already
/// sent are never dropped. A cancelled entry whose `cancelled_by` is missing
/// or unreadable ends as `abandoned`, as a poll ends it, because
/// `end_entry` would refuse a `cancelled` row without a canceller and fail
/// every advance.
export auto advance_terminations(db::connection& conn, const advance_request& request, process::identity::clock& clock,
                                 const process_probe& probe, const group_signaller& signaller)
    -> std::expected<advance_result, queue_error>;

/// @brief What the polling submitter supplies to `poll_and_stop`.
export struct poll_stop_request {
  poll_request poll;         ///< The poll: the caller's entry, host identity, slot count, window and run limit.
  std::int64_t grace_ms = 0; ///< How long an entry may be terminating before SIGKILL; must not be negative.
};

/// @brief What `poll_and_stop` did.
export struct poll_stop_result {
  poll_result                 poll;          ///< The poll's result; when it was skipped, nothing else was done.
  std::vector<signal_attempt> sigterms;      ///< One SIGTERM attempt per entry the poll marked, in the poll's order.
  advance_result              advanced;      ///< What `advance_terminations` did; empty when it did not run.
  std::optional<queue_error>  advance_error; ///< Why `advance_terminations` failed, when it did; the poll still committed.
};

/// @brief Runs one poll and then the stopping steps it leads to, as this
/// module's description states: `poll`, then, with the poll committed and no
/// transaction open, SIGTERM to each entry it marked, then
/// `advance_terminations` over every terminating entry on this host.
/// @param conn An open agent database at or above agent schema version 2,
/// not inside a transaction.
/// @param request The poll request and the grace period.
/// @param clock The monotonic and wall clocks, read by the poll and by the
/// advance.
/// @param probe The process queries.
/// @param signaller Where SIGTERM and SIGKILL go.
/// @return The result, including a skipped poll (after which nothing was
/// signalled or advanced) and an advance failure (reported on the result,
/// because the poll it follows has committed); `invalid_request` for a
/// negative grace period, before anything is done; or the poll's own error,
/// after which nothing was changed or signalled.
export auto poll_and_stop(db::connection& conn, const poll_stop_request& request, process::identity::clock& clock,
                          const process_probe& probe, const group_signaller& signaller)
    -> std::expected<poll_stop_result, queue_error>;

} // namespace planar::engine::hostqueue
