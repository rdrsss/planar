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

} // namespace planar::engine::hostqueue
