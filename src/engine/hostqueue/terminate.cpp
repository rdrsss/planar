/// @file terminate.cpp
/// @brief Implementation of `planar.engine.hostqueue.terminate` (plan 1080,
/// task hq-terminate). See terminate.cppm for the contract.

module;

#include <csignal>

module planar.engine.hostqueue.terminate;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.history;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

auto to_string(stop_reason reason) -> std::string_view {
  switch (reason) {
  case stop_reason::timeout:
    return "timeout";
  case stop_reason::cancelled:
    return "cancelled";
  }
  return "timeout";
}

auto signal_number(stop_signal sig) -> int {
  return sig == stop_signal::kill ? SIGKILL : SIGTERM;
}

auto system_group_signaller() -> group_signaller {
  return [](std::int64_t pgid, int sig) { return process::identity::signal_group(pgid, sig); };
}

auto signal_child_group(const entry& e, stop_signal sig, std::string_view /*host_id*/, const process_probe& /*probe*/,
                        const group_signaller& /*signaller*/) -> signal_attempt {
  // Red stub: sends nothing.
  return signal_attempt{.seq = e.seq, .signal = sig, .outcome = signal_outcome::no_group, .error = std::nullopt};
}

auto begin_terminate(db::connection& /*conn*/, const begin_terminate_request& /*request*/, process::identity::clock& /*clock*/,
                     const process_probe& /*probe*/, const group_signaller& /*signaller*/)
    -> std::expected<begin_result, queue_error> {
  // Red stub: does nothing.
  return begin_result{};
}

auto advance_terminations(db::connection& /*conn*/, const advance_request& /*request*/, process::identity::clock& /*clock*/,
                          const process_probe& /*probe*/, const group_signaller& /*signaller*/)
    -> std::expected<advance_result, queue_error> {
  // Red stub: does nothing.
  return advance_result{};
}

} // namespace planar::engine::hostqueue
