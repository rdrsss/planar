/// @file nested.cpp
/// @brief Implementation of `planar.engine.hostqueue.nested` (plan 1080, task
/// hq-nested-entry). See nested.cppm for the contract.

module planar.engine.hostqueue.nested;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

auto enqueue_nested(db::connection& conn, std::int64_t parent_seq, const enqueue_request& request, const nested_limits& limits,
                    process::identity::clock& clock, const process_probe& probe) -> std::expected<nested_result, queue_error> {
  (void)limits;
  (void)clock;
  (void)probe;
  auto nested       = request;
  nested.parent_seq = parent_seq;
  auto seq          = enqueue(conn, nested);
  if (!seq) {
    return std::unexpected(std::move(seq.error()));
  }
  return nested_result{.status = nested_status::inserted, .seq = *seq};
}

} // namespace planar::engine::hostqueue
