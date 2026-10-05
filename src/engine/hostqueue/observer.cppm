/// @file observer.cppm
/// @brief Typed decisions for observing a logical host-queue submission.
/// The observer consumes read-only status snapshots and never changes queue
/// entries, history, claims or the submitted command. Deadline and signal
/// ownership belong to the caller; all times here are monotonic milliseconds.
module;

export module planar.engine.hostqueue.observer;

import std;
import planar.engine.hostqueue.status;

namespace planar::engine::hostqueue {

/// @brief Why a logical-job observation continues or stops.
export enum class wait_reason : std::uint8_t {
  pending,             ///< Another snapshot or a caller-owned deadline is needed.
  completed,           ///< Authoritative history records a final outcome.
  stalled,             ///< The same active sequence was confirmed dead at least one second apart.
  history_unavailable, ///< The initial ticket or an explicit successor cannot be read.
  error,               ///< A malformed successor chain, history row or clock was observed.
};

/// @brief One decision from a strict queue status snapshot.
export struct wait_transition {
  wait_reason                     reason = wait_reason::pending; ///< Continue or stop reason.
  std::optional<int>              result_exit_code;              ///< Process exit mapping when this decision stops.
  std::optional<std::string_view> tag;                           ///< Stable diagnostic for uncertainty or malformed data.
};

/// @brief Evaluates successive typed snapshots; it holds only dead-entry confirmation state.
/// One instance belongs to one wait invocation. It is not thread-safe and owns no store handle.
export class wait_observer {
public:
  /// @brief Evaluates one fresh snapshot and updates dead-entry confirmation.
  /// @param snapshot The strict result of `query_wait_status` for the same requested ticket.
  /// @param now_mono_ms The current monotonic time in milliseconds.
  /// @return A pending or terminal typed decision.
  auto step(const wait_status_lookup& snapshot, std::int64_t now_mono_ms) -> wait_transition;

private:
  std::optional<std::int64_t> _dead_seq;
  std::optional<std::int64_t> _dead_since_ms;
  std::optional<std::int64_t> _last_mono_ms;
};

} // namespace planar::engine::hostqueue
