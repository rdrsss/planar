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

namespace {

/// @brief Wraps a `planar.db` failure as a `query_failed` error naming the
/// operation it interrupted.
auto sql_failure(std::string_view what, const db::db_error& err) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::query_failed,
      .sqlite_code = err.code_,
      .message     = std::format("hostqueue: {}: {} (sqlite {})", what, err.message_, err.code_),
  });
}

/// @brief An `invalid_request` error with `message`.
auto invalid(std::string_view message) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::invalid_request,
      .sqlite_code = 0,
      .message     = std::format("hostqueue: nested enqueue: {}", message),
  });
}

/// @brief `base + span`, saturating instead of overflowing.
auto saturating_add(std::int64_t base, std::int64_t span) -> std::int64_t {
  auto const max = std::numeric_limits<std::int64_t>::max();
  return base > max - span ? max : base + span;
}

/// @brief A refusal result: nothing inserted, the caller queues normally.
auto refused(nested_refusal why, std::int64_t now) -> nested_result {
  return nested_result{.status = nested_status::queue_normally, .refusal = why, .now_mono = now};
}

/// @brief Sets the start time, the deadline and the run limit the deadline
/// was computed from on the just-inserted entry `seq`.
auto set_started(db::connection& conn, std::int64_t seq, std::int64_t started_at, std::int64_t deadline_mono,
                 std::int64_t run_limit_ms) -> std::expected<void, queue_error> {
  auto stmt = conn.prepare("update queue_entries set started_at = ?, deadline_mono = ?, run_limit_ms = ? where seq = ?");
  if (!stmt) {
    return sql_failure("prepare nested start", stmt.error());
  }
  std::array<std::expected<void, db::db_error>, 4> const bound{{
      stmt->bind_int64(1, started_at),
      stmt->bind_int64(2, deadline_mono),
      stmt->bind_int64(3, run_limit_ms),
      stmt->bind_int64(4, seq),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind nested start", result.error());
    }
  }
  if (auto stepped = stmt->step(); !stepped) {
    return sql_failure("start nested entry", stepped.error());
  }
  return {};
}

} // namespace

auto enqueue_nested(db::connection& conn, std::int64_t parent_seq, const enqueue_request& request, const nested_limits& limits,
                    process::identity::clock& clock, const process_probe& probe) -> std::expected<nested_result, queue_error> {
  if (limits.stale_after_ms < 0) {
    return invalid(std::format("staleness window must not be negative, got {} ms", limits.stale_after_ms));
  }
  if (limits.run_limit_ms < 0) {
    return invalid(std::format("run limit must not be negative, got {} ms", limits.run_limit_ms));
  }
  if (request.parent_seq && *request.parent_seq != parent_seq) {
    return invalid(std::format("the request names parent {}, the call names parent {}", *request.parent_seq, parent_seq));
  }
  if (conn.in_transaction()) {
    // A savepoint takes no write lock of its own, so the parent check and
    // the insert would not be serialised against a poll that ends the parent.
    return invalid("the connection is already in a transaction");
  }

  auto txn = conn.begin_transaction(db::lock_mode::immediate);
  if (!txn) {
    return sql_failure("begin nested enqueue", txn.error());
  }

  // Decision 1203: the monotonic clock is read only once the write lock is
  // held, so no refresh of the parent can commit later than this time.
  auto const now = clock.monotonic_ms();
  if (!now) {
    return std::unexpected(queue_error{
        .kind        = queue_error_kind::clock_failed,
        .sqlite_code = 0,
        .message     = "hostqueue: nested enqueue: the monotonic clock could not be read",
    });
  }

  auto parent = find(conn, parent_seq);
  if (!parent) {
    return std::unexpected(std::move(parent.error()));
  }
  // A refusal writes nothing; the transaction is rolled back when `txn`
  // leaves scope.
  if (!parent->has_value()) {
    return refused(nested_refusal::parent_missing, *now);
  }
  if ((*parent)->state != entry_state::running) {
    return refused(nested_refusal::parent_waiting, *now);
  }
  liveness_context const ctx{.host_id = request.host_id, .now_mono = *now, .stale_after_ms = limits.stale_after_ms};
  auto                   verdict = judge_liveness(**parent, ctx, probe);
  if (!verdict) {
    return refused(nested_refusal::parent_unjudged, *now);
  }
  if (!verdict->live) {
    return refused(nested_refusal::parent_not_live, *now);
  }

  auto nested           = request;
  nested.parent_seq     = parent_seq;
  nested.refreshed_mono = *now;
  auto seq              = enqueue(conn, nested);
  if (!seq) {
    return std::unexpected(std::move(seq.error()));
  }
  auto const started_at = clock.wall_ms();
  auto const deadline   = saturating_add(*now, limits.run_limit_ms);
  if (auto started = set_started(conn, *seq, started_at, deadline, limits.run_limit_ms); !started) {
    return std::unexpected(std::move(started.error()));
  }

  if (auto committed = txn->commit(); !committed) {
    return sql_failure("commit nested enqueue", committed.error());
  }
  return nested_result{
      .status        = nested_status::inserted,
      .refusal       = std::nullopt,
      .seq           = *seq,
      .now_mono      = *now,
      .started_at    = started_at,
      .deadline_mono = deadline,
  };
}

} // namespace planar::engine::hostqueue
