/// @file poll.cpp
/// @brief Implementation of `planar.engine.hostqueue.poll` (plan 1080, task
/// hq-poll-transaction). See poll.cppm for the contract.

module planar.engine.hostqueue.poll;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

auto poll(db::connection& /*conn*/, const poll_request& /*request*/, process::identity::clock& /*clock*/,
          const process_probe& /*probe*/) -> std::expected<poll_result, queue_error> {
  // Red stub: does nothing.
  return poll_result{};
}

} // namespace planar::engine::hostqueue
