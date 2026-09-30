/// @file liveness.cppm
/// @brief `planar.engine.hostqueue.liveness` — whether a queue entry is
/// live, and which entries a poll reaps (plan 1080, task hq-liveness; tech
/// spec 647 § Liveness, precisely).
///
/// Liveness is a pure judgement over one `entry` and a `liveness_context`
/// (the checker's host identity, the current monotonic time and the
/// staleness window). Every process query goes through a `process_probe`,
/// whose default (`system_process_probe`) forwards to
/// `planar.process.identity`; a test hands in a fake to drive every branch.
/// Nothing here reads the store, writes it, or sends a signal.
///
/// The rules, from the tech spec:
///
/// * A `waiting` entry is live when the submitter's process exists, its
///   start time matches the recorded one, and the entry was refreshed
///   within the staleness window.
/// * A `running` entry is live when the `waiting` test passes, or the child
///   group has at least one member.
///
/// * Existence is `kill(pid, 0)`, with `EPERM` counting as existing (the
///   probe's `process_exists`).
/// * A child group id is reused when a process whose id equals `child_pgid`
///   exists and its start time differs from `child_started`: the group is
///   then treated as empty and is reported as `group_verdict::reused`, so a
///   caller can refuse to signal it.
/// * An entry whose `host_id` differs from the checker's (the literal
///   `unknown` on either side included) is never tested by process id. It is
///   judged by freshness alone, and its group is `not_checked`.
/// * A submitter is gone when its entry fails the `waiting` test.
///
/// Freshness is `|now - refreshed_mono| <= stale_after`. A refresh time
/// ahead of `now` by no more than the window is a concurrent refresh that
/// committed after the checker read its clock; one ahead by more than the
/// window comes from a clock this checker does not share (an earlier boot,
/// whose monotonic clock ran longer than the current one has), and is
/// stale.
///
/// Error boundary: a probe failure is returned as
/// `planar::process::identity::error` from `judge_liveness`. `select_not_live`
/// never selects an entry whose judgement failed, so a failing query can
/// delay a reap but can never reap a live entry.
export module planar.engine.hostqueue.liveness;

import std;
import planar.process.identity;
import planar.engine.hostqueue.queue;

namespace planar::engine::hostqueue {

/// @brief The process queries liveness needs, as replaceable functions.
///
/// Each member has the signature and meaning of the
/// `planar.process.identity` function of the same name. Every member must be
/// set; `system_process_probe` sets them to those functions.
export struct process_probe {
  /// @brief Whether a process with this id exists (`EPERM` counts as existing).
  std::function<std::expected<bool, process::identity::error>(std::int64_t)> process_exists;
  /// @brief The start time of the process with this id, `std::nullopt` when absent.
  std::function<std::expected<std::optional<process::identity::start_time>, process::identity::error>(std::int64_t)>
      process_start_time;
  /// @brief Whether the process group with this id has at least one member.
  std::function<std::expected<bool, process::identity::error>(std::int64_t)> group_has_members;
  /// @brief Whether the process group with this id has members and every one of them is an exited process
  /// that has not been reaped. Left empty (as a test's fake leaves it), it answers "no".
  std::function<std::expected<bool, process::identity::error>(std::int64_t)> group_only_zombies;
};

/// @brief Whether `probe` says the group `pgid` holds nothing but unreaped
/// exited processes. An unset probe member, or a failed query, is "no": the
/// caller then treats the group as it would have without the question.
/// @param probe The process queries.
/// @param pgid The group id.
/// @return True only when the probe verified it.
export auto group_has_only_zombies(const process_probe& probe, std::int64_t pgid) -> bool;

/// @brief The probe that asks the host, through `planar.process.identity`.
/// @return A probe whose members forward to `process_exists`,
/// `process_start_time` and `group_has_members`.
export auto system_process_probe() -> process_probe;

/// @brief What the checking process knows when it judges an entry.
export struct liveness_context {
  std::string  host_id;            ///< The checker's own host identity (`unknown` when unreadable).
  std::int64_t now_mono       = 0; ///< The checker's monotonic clock, ms; the clock that does not advance while asleep.
  std::int64_t stale_after_ms = 0; ///< The staleness window, ms.
};

/// @brief What is known about a running entry's child group.
export enum class group_verdict : std::uint8_t {
  not_checked, ///< Not examined: the entry is waiting, records no child group, or is from another host identity.
  has_members, ///< The group has at least one member.
  empty,       ///< The group has no member.
  reused,      ///< A process with the group's id exists with a different start time; treated as empty, never to be signalled.
};

/// @brief The judgement of one entry.
export struct liveness {
  bool          live           = false;                      ///< Whether the entry passes the liveness rules for its state.
  bool          submitter_live = false;                      ///< Whether the entry passes the `waiting` test.
  group_verdict group          = group_verdict::not_checked; ///< The child group's state, for a running entry on this host.

  /// @brief Whether the submitter is gone: the entry fails the `waiting` test.
  /// @return `!submitter_live`.
  [[nodiscard]] auto submitter_gone() const -> bool {
    return !submitter_live;
  }
};

/// @brief Whether `refreshed_mono` is within the staleness window of `ctx.now_mono`.
/// @param refreshed_mono The entry's last refresh, monotonic ms.
/// @param ctx The checker's clock and window.
/// @return `true` when `|now - refreshed| <= stale_after`.
export auto is_fresh(std::int64_t refreshed_mono, const liveness_context& ctx) -> bool;

/// @brief Judges one entry by the rules in this module's description.
/// @param e The entry.
/// @param ctx The checker's host identity, clock and staleness window.
/// @param probe The process queries.
/// @return The judgement, or the first probe failure.
export auto judge_liveness(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<liveness, process::identity::error>;

/// @brief The state of an entry's child group, by the same rule
/// `judge_liveness` applies: `reused` when a process with the id `child_pgid`
/// exists with a start time other than `child_started`, otherwise whether the
/// group has members. The host identity is not compared; the caller does
/// that before trusting any process id.
/// @param e The entry.
/// @param probe The process queries.
/// @return `not_checked` for an entry that is not running or records no
/// child group; otherwise the verdict, or the first probe failure.
export auto judge_child_group(const entry& e, const process_probe& probe)
    -> std::expected<group_verdict, process::identity::error>;

/// @brief Whether the entry's submitter is gone (it fails the `waiting` test).
/// @param e The entry, waiting or running.
/// @param ctx The checker's host identity, clock and staleness window.
/// @param probe The process queries.
/// @return `true` when gone, or the first probe failure.
export auto submitter_gone(const entry& e, const liveness_context& ctx, const process_probe& probe)
    -> std::expected<bool, process::identity::error>;

/// @brief The sequence numbers of the entries a poll reaps: those that are
/// not live. Nothing is deleted.
/// @param entries The entries to judge.
/// @param ctx The checker's host identity, clock and staleness window.
/// @param probe The process queries.
/// @return The sequence numbers of entries judged not live, in the order
/// given. An entry whose judgement failed is not included.
export auto select_not_live(std::span<entry const> entries, const liveness_context& ctx, const process_probe& probe)
    -> std::vector<std::int64_t>;

} // namespace planar::engine::hostqueue
