/// @file poll.cpp
/// @brief Implementation of `planar.engine.hostqueue.poll` (plan 1080, task
/// hq-poll-transaction). See poll.cppm for the contract.

module planar.engine.hostqueue.poll;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.history;
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
      .message     = std::format("hostqueue: poll: {}", message),
  });
}

/// @brief Sets `refreshed_mono` on the caller's entry.
/// @return Whether the entry exists.
auto refresh(db::connection& conn, std::int64_t seq, std::int64_t now) -> std::expected<bool, queue_error> {
  auto stmt = conn.prepare("update queue_entries set refreshed_mono = ? where seq = ? returning seq");
  if (!stmt) {
    return sql_failure("prepare refresh", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, now); !bound) {
    return sql_failure("bind refresh", bound.error());
  }
  if (auto bound = stmt->bind_int64(2, seq); !bound) {
    return sql_failure("bind refresh", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("refresh queue entry", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return false;
  }
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish refresh", done.error());
  }
  return true;
}

/// @brief Records the terminating marker (reason `timeout`) on an entry
/// that is not already terminating.
/// @return Whether this call set the marker.
auto mark_terminating(db::connection& conn, std::int64_t seq, std::int64_t now) -> std::expected<bool, queue_error> {
  auto stmt = conn.prepare("update queue_entries set terminating_since_mono = ?, terminate_reason = 'timeout' "
                           "where seq = ? and terminating_since_mono is null returning seq");
  if (!stmt) {
    return sql_failure("prepare terminating mark", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, now); !bound) {
    return sql_failure("bind terminating mark", bound.error());
  }
  if (auto bound = stmt->bind_int64(2, seq); !bound) {
    return sql_failure("bind terminating mark", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("mark entry terminating", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return false;
  }
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish terminating mark", done.error());
  }
  return true;
}

/// @brief Sets a waiting entry `running` with its start time, its deadline
/// and the run limit the deadline was computed from, in one statement so the
/// recorded limit and the deadline cannot disagree.
auto start_entry(db::connection& conn, std::int64_t seq, std::int64_t started_at, std::int64_t deadline_mono,
                 std::int64_t run_limit_ms) -> std::expected<void, queue_error> {
  auto stmt = conn.prepare("update queue_entries set state = 'running', started_at = ?, deadline_mono = ?, run_limit_ms = ? "
                           "where seq = ? and state = 'waiting'");
  if (!stmt) {
    return sql_failure("prepare start", stmt.error());
  }
  std::array<std::expected<void, db::db_error>, 4> const bound{{
      stmt->bind_int64(1, started_at),
      stmt->bind_int64(2, deadline_mono),
      stmt->bind_int64(3, run_limit_ms),
      stmt->bind_int64(4, seq),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind start", result.error());
    }
  }
  if (auto stepped = stmt->step(); !stepped) {
    return sql_failure("start queue entry", stepped.error());
  }
  return {};
}

/// @brief How a terminating entry that is reaped ends: the outcome its
/// `terminate_reason` names, and for `cancelled` the canceller it records.
struct stop_end {
  history_outcome          outcome = history_outcome::timeout; ///< `timeout` or `cancelled`.
  std::optional<canceller> who;                                ///< The recorded canceller, for `cancelled`.
};

/// @brief The end of a reaped entry whose stop was under way, or
/// `std::nullopt` when none was: the entry carries no terminating marker, or
/// (for a row not written by `begin_terminate`) a cancellation without a
/// readable canceller, which would make `end_entry` refuse the row and so
/// fail every poll.
auto stopped_end(const entry& e) -> std::optional<stop_end> {
  if (!e.terminating_since_mono || !e.terminate_reason) {
    return std::nullopt;
  }
  if (*e.terminate_reason == "timeout") {
    return stop_end{.outcome = history_outcome::timeout, .who = std::nullopt};
  }
  if (*e.terminate_reason == "cancelled" && e.cancelled_by) {
    if (auto who = decode_canceller(*e.cancelled_by); who) {
      return stop_end{.outcome = history_outcome::cancelled, .who = std::move(*who)};
    }
  }
  return std::nullopt;
}

/// @brief `base + span`, saturating instead of overflowing.
auto saturating_add(std::int64_t base, std::int64_t span) -> std::int64_t {
  auto const max = std::numeric_limits<std::int64_t>::max();
  return base > max - span ? max : base + span;
}

} // namespace

auto poll(db::connection& conn, const poll_request& request, process::identity::clock& clock, const process_probe& probe)
    -> std::expected<poll_result, queue_error> {
  if (request.slots < 0) {
    return invalid(std::format("slot count must not be negative, got {}", request.slots));
  }
  if (request.stale_after_ms < 0) {
    return invalid(std::format("staleness window must not be negative, got {} ms", request.stale_after_ms));
  }
  if (request.run_limit_ms < 0) {
    return invalid(std::format("run limit must not be negative, got {} ms", request.run_limit_ms));
  }
  if (conn.in_transaction()) {
    // A nested scope would be a savepoint, which takes no write lock of its
    // own: the turn check would no longer be serialised against other polls.
    return invalid("the connection is already in a transaction");
  }

  auto txn = conn.begin_transaction(db::lock_mode::immediate);
  if (!txn) {
    if (db::is_busy(txn.error())) {
      return poll_result{.status = poll_status::skipped};
    }
    return sql_failure("begin poll", txn.error());
  }

  // Decision 1203: the monotonic clock is read only once the write lock is
  // held, so no refresh can commit later than the time compared against.
  auto const now = clock.monotonic_ms();
  if (!now) {
    return std::unexpected(queue_error{
        .kind        = queue_error_kind::clock_failed,
        .sqlite_code = 0,
        .message     = "hostqueue: poll: the monotonic clock could not be read",
    });
  }
  auto const wall = clock.wall_ms();

  poll_result result{.status = poll_status::completed, .now_mono = *now};

  // Step 1: refresh the caller's own entry.
  auto found = refresh(conn, request.seq, *now);
  if (!found) {
    return std::unexpected(std::move(found.error()));
  }
  result.entry_missing = !*found;

  auto entries = list(conn);
  if (!entries) {
    return std::unexpected(std::move(entries.error()));
  }

  liveness_context const ctx{.host_id = request.host_id, .now_mono = *now, .stale_after_ms = request.stale_after_ms};

  // The entries still standing after the reap, lowest sequence number first:
  // the caller's own, every live one, and every one that could not be judged.
  std::vector<const entry*> standing;
  standing.reserve(entries->size());
  for (auto const& e : *entries) {
    if (e.seq == request.seq) {
      // The calling process is this entry's submitter and has just refreshed
      // it; it is never judged.
      standing.push_back(&e);
      continue;
    }
    auto verdict = judge_liveness(e, ctx, probe);
    if (!verdict) {
      result.liveness_errors.push_back(liveness_failure{.seq = e.seq, .error = verdict.error()});
      standing.push_back(&e);
      continue;
    }

    // Step 2: reap an entry that is not live. One whose stop was already
    // under way ends with the outcome its terminate reason names (and its
    // canceller), not `abandoned`.
    if (!verdict->live) {
      auto const  stop = stopped_end(e);
      end_request request_end{.outcome = history_outcome::abandoned, .ended_at = wall};
      if (stop) {
        request_end.outcome      = stop->outcome;
        request_end.cancelled_by = stop->who;
      }
      auto ended = end_entry(conn, e.seq, request_end);
      if (!ended) {
        return std::unexpected(std::move(ended.error()));
      }
      if (*ended == end_result::ended) {
        if (stop) {
          result.stopped.push_back(stopped_entry{.seq = e.seq, .outcome = stop->outcome});
        } else {
          result.reaped.push_back(e.seq);
        }
      }
      continue;
    }
    standing.push_back(&e);

    // Step 3: mark an overdue running entry whose submitter is gone. Only
    // the marker is written; signals are the caller's, after the commit.
    bool const overdue = e.deadline_mono && *now > *e.deadline_mono;
    if (e.state == entry_state::running && overdue && !e.terminating_since_mono && verdict->submitter_gone()) {
      auto marked = mark_terminating(conn, e.seq, *now);
      if (!marked) {
        return std::unexpected(std::move(marked.error()));
      }
      if (*marked) {
        entry updated                  = e;
        updated.terminating_since_mono = *now;
        updated.terminate_reason       = "timeout";
        result.terminating.push_back(std::move(updated));
      }
    }
  }

  // Step 4: the caller's turn. Only a waiting entry is started; a running
  // one keeps the start time and deadline it was given.
  if (!result.entry_missing) {
    auto const own = std::ranges::find_if(standing, [&](const entry* e) { return e->seq == request.seq; });
    if (own != standing.end()) {
      entry const& mine = **own;
      if (mine.state == entry_state::waiting) {
        std::int64_t ahead = 0;
        for (auto const* e : standing) {
          if (e->seq < mine.seq && !e->parent_seq) {
            ++ahead;
          }
        }
        if (ahead < request.slots) {
          auto const deadline = saturating_add(*now, request.run_limit_ms);
          if (auto started = start_entry(conn, mine.seq, wall, deadline, request.run_limit_ms); !started) {
            return std::unexpected(std::move(started.error()));
          }
          result.running       = true;
          result.started       = true;
          result.started_at    = wall;
          result.deadline_mono = deadline;
        }
      } else {
        result.running       = true;
        result.started_at    = mine.started_at;
        result.deadline_mono = mine.deadline_mono;
      }
    }
  }

  if (auto committed = txn->commit(); !committed) {
    return sql_failure("commit poll", committed.error());
  }
  return result;
}

} // namespace planar::engine::hostqueue
