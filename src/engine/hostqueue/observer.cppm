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
  timed_out,           ///< The observer's own finite deadline expired.
  interrupted,         ///< A caller-owned interruption stopped observation.
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

/// @brief Synchronous, injectable operations for one bounded observation.
/// Callbacks must remain alive for the call. `read` must call its supplied
/// remaining-budget function before each store read and cap SQLite busy waits
/// to that result; it must not retain a statement or transaction after return.
export struct wait_runtime {
  std::function<std::optional<std::int64_t>()> now_ms;             ///< Monotonic milliseconds, or unavailable.
  std::function<std::optional<int>()>          interrupted_signal; ///< Pending signal, if any.
  std::function<std::expected<wait_status_lookup, status_error>(const std::function<std::expected<int, status_error>()>&)>
                                                 read;  ///< One fresh status lookup using the remaining-budget check.
  std::function<void(std::chrono::milliseconds)> sleep; ///< Interruptible wait of at most the supplied span.
};

/// @brief Final observation, retaining the last authoritative snapshot even on timeout.
export struct wait_result {
  wait_reason                       reason = wait_reason::error; ///< Why observation stopped.
  std::optional<int>                result_exit_code;            ///< Observer or recorded child exit mapping.
  std::optional<std::string>        tag;                         ///< Stable diagnostic tag for error or uncertainty.
  std::optional<wait_status_lookup> snapshot;                    ///< Last committed status observed, if any.
  std::optional<status_error>       error;                       ///< Store or settings error, if any.
  std::int64_t                      elapsed_ms = 0;              ///< Monotonic elapsed time since pre-open start.
};

/// @brief Observes one logical ticket within a finite monotonic budget.
/// @param timeout_ms Positive duration, no greater than 24 hours.
/// @param runtime Caller-owned synchronous clocks, interruption, read and sleep operations.
/// @param started_mono_ms Monotonic start captured before opening the store.
/// @return One stop reason; timeout/interruption never changes the queued job.
export auto observe_wait(std::int64_t timeout_ms, const wait_runtime& runtime, std::int64_t started_mono_ms) -> wait_result;

/// @brief Computes a checked monotonic deadline before any store open.
/// @param timeout_ms Positive finite observation budget in milliseconds.
/// @param started_mono_ms Monotonic start captured before opening the store.
/// @return The deadline, or a stable error tag.
export auto checked_wait_deadline(std::int64_t timeout_ms, std::int64_t started_mono_ms)
    -> std::expected<std::int64_t, std::string_view>;

} // namespace planar::engine::hostqueue
