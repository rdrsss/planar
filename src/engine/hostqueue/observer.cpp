/// @file observer.cpp
/// @brief Implementation of typed logical-job observer transitions.

module planar.engine.hostqueue.observer;

import std;
import planar.engine.hostqueue.status;
import planar.engine.hostqueue.history;

namespace planar::engine::hostqueue {

namespace {

auto completed_exit(const queue_status& status) -> wait_transition {
  if (!status.outcome) {
    return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_history"};
  }
  switch (*status.outcome) {
  case history_outcome::exited:
  case history_outcome::not_started:
    if (!status.exit_code || *status.exit_code < 0 || *status.exit_code > 255) {
      return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_history"};
    }
    if (*status.outcome == history_outcome::not_started && *status.exit_code != 126 && *status.exit_code != 127) {
      return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_history"};
    }
    return {.reason = wait_reason::completed, .result_exit_code = static_cast<int>(*status.exit_code)};
  case history_outcome::signaled:
    if (!status.signal || *status.signal <= 0 || *status.signal > 127) {
      return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_history"};
    }
    return {.reason = wait_reason::completed, .result_exit_code = static_cast<int>(128 + *status.signal)};
  case history_outcome::timeout:
    return {.reason = wait_reason::completed, .result_exit_code = 124};
  case history_outcome::cancelled:
  case history_outcome::wait_timeout:
    return {.reason = wait_reason::completed, .result_exit_code = 125};
  case history_outcome::abandoned:
    return {};
  }
  return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_history"};
}

} // namespace

auto wait_observer::step(const wait_status_lookup& snapshot, std::int64_t now_mono_ms) -> wait_transition {
  if (_last_mono_ms && now_mono_ms < *_last_mono_ms) {
    return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "clock_backwards"};
  }
  _last_mono_ms = now_mono_ms;

  if (snapshot.issue == wait_lookup_issue::invalid_successor) {
    return {.reason = wait_reason::error, .result_exit_code = 125, .tag = "invalid_successor"};
  }
  if (snapshot.issue == wait_lookup_issue::successor_history_unavailable) {
    return {.reason = wait_reason::history_unavailable, .result_exit_code = 1, .tag = "successor_history_unavailable"};
  }
  if (!snapshot.status) {
    return {.reason = wait_reason::history_unavailable, .result_exit_code = 1, .tag = "not_found"};
  }
  if (snapshot.status->state == status_state::ended) {
    _dead_seq.reset();
    _dead_since_ms.reset();
    return completed_exit(*snapshot.status);
  }

  if (!snapshot.observed_seq || snapshot.status->live != std::optional<bool>{false}) {
    _dead_seq.reset();
    _dead_since_ms.reset();
    return {};
  }
  if (_dead_seq != snapshot.observed_seq || !_dead_since_ms) {
    _dead_seq      = snapshot.observed_seq;
    _dead_since_ms = now_mono_ms;
    return {};
  }
  if (static_cast<std::uint64_t>(now_mono_ms) - static_cast<std::uint64_t>(*_dead_since_ms) >= 1'000) {
    return {.reason = wait_reason::stalled, .result_exit_code = 125, .tag = "stalled"};
  }
  return {};
}

auto checked_wait_deadline(std::int64_t timeout_ms, std::int64_t started_mono_ms)
    -> std::expected<std::int64_t, std::string_view> {
  if (timeout_ms <= 0 || timeout_ms > 86'400'000) {
    return std::unexpected("invalid_observer_runtime");
  }
  if (started_mono_ms > std::numeric_limits<std::int64_t>::max() - timeout_ms) {
    return std::unexpected("clock_overflow");
  }
  return started_mono_ms + timeout_ms;
}

auto observe_wait(std::int64_t timeout_ms, const wait_runtime& runtime, std::int64_t started_mono_ms) -> wait_result {
  wait_result result;
  result.result_exit_code = 125;
  if (!runtime.now_ms || !runtime.interrupted_signal || !runtime.read || !runtime.sleep) {
    result.tag = "invalid_observer_runtime";
    return result;
  }
  auto const deadline_result = checked_wait_deadline(timeout_ms, started_mono_ms);
  if (!deadline_result) {
    result.tag = std::string{deadline_result.error()};
    return result;
  }
  auto const deadline = *deadline_result;
  auto       last_now = started_mono_ms;
  auto const clock    = [&]() -> std::optional<std::int64_t> {
    auto now = runtime.now_ms();
    if (!now || *now < last_now) {
      result.reason           = wait_reason::error;
      result.tag              = now ? "clock_backwards" : "clock_unavailable";
      result.result_exit_code = 125;
      return std::nullopt;
    }
    last_now          = *now;
    result.elapsed_ms = *now - started_mono_ms;
    return now;
  };
  auto const check = [&]() -> std::expected<int, status_error> {
    if (auto signal = runtime.interrupted_signal()) {
      result.reason           = wait_reason::interrupted;
      result.result_exit_code = 128 + *signal;
      return std::unexpected(status_error{.kind = status_error_kind::store, .message = "observation interrupted"});
    }
    auto now = clock();
    if (!now) {
      return std::unexpected(status_error{.kind = status_error_kind::store, .message = "monotonic clock failed"});
    }
    if (*now >= deadline) {
      result.reason           = wait_reason::timed_out;
      result.result_exit_code = 124;
      return std::unexpected(status_error{.kind = status_error_kind::store, .message = "observation deadline expired"});
    }
    return static_cast<int>(std::min<std::int64_t>(deadline - *now, std::numeric_limits<int>::max()));
  };

  wait_observer observer;
  for (;;) {
    if (!check()) {
      return result;
    }
    auto snapshot = runtime.read(check);
    // A busy read can exhaust the observation budget. Its expiry takes
    // precedence over SQLite's error, but a completed read is still judged.
    if (!snapshot) {
      if (!check()) {
        return result;
      }
      result.error            = std::move(snapshot.error());
      result.tag              = "store_unreadable";
      result.result_exit_code = 125;
      return result;
    }
    result.snapshot = std::move(*snapshot);
    auto now        = clock();
    if (!now) {
      return result;
    }
    if (*now >= deadline) {
      result.reason           = wait_reason::timed_out;
      result.result_exit_code = 124;
      return result;
    }
    auto const decision = observer.step(*result.snapshot, *now);
    if (decision.reason != wait_reason::pending) {
      result.reason           = decision.reason;
      result.result_exit_code = decision.result_exit_code;
      if (decision.tag) {
        result.tag = std::string{*decision.tag};
      }
      return result;
    }
    if (!check()) {
      return result;
    }
    runtime.sleep(std::chrono::milliseconds{std::min<std::int64_t>(1'000, deadline - last_now)});
  }
}

} // namespace planar::engine::hostqueue
