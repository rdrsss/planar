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
import planar.engine.hostqueue.poll;

namespace planar::engine::hostqueue {

namespace {

namespace identity = process::identity;

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
auto invalid(std::string_view operation, std::string_view message) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::invalid_request,
      .sqlite_code = 0,
      .message     = std::format("hostqueue: {}: {}", operation, message),
  });
}

/// @brief A `clock_failed` error for `operation`.
auto clock_failure(std::string_view operation) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::clock_failed,
      .sqlite_code = 0,
      .message     = std::format("hostqueue: {}: the monotonic clock could not be read", operation),
  });
}

/// @brief Whether the checker may use this entry's process ids: both host
/// identities are known and equal (the rule liveness applies).
auto same_host(const entry& e, std::string_view host_id) -> bool {
  return e.host_id == host_id && e.host_id != identity::k_unknown_host_identity;
}

/// @brief Whether the entry records a child group that can be verified: an
/// id above 1 (never the caller's own group, init's, or every process) and
/// the start time of the group's leader.
auto verifiable_group(const entry& e) -> bool {
  return e.child_pgid && *e.child_pgid > 1 && e.child_started;
}

/// @brief Records the terminating marker on a running entry that carries
/// none.
/// @return Whether this call set it.
auto mark(db::connection& conn, const begin_terminate_request& request, std::int64_t now) -> std::expected<bool, queue_error> {
  auto stmt = conn.prepare("update queue_entries set terminating_since_mono = ?, terminate_reason = ?, cancelled_by = ? "
                           "where seq = ? and state = 'running' and terminating_since_mono is null returning seq");
  if (!stmt) {
    return sql_failure("prepare terminating mark", stmt.error());
  }
  std::expected<void, db::db_error> const who =
      request.cancelled_by ? stmt->bind_text(3, encode_canceller(*request.cancelled_by)) : stmt->bind_null(3);
  std::array<std::expected<void, db::db_error>, 4> const bound{{
      stmt->bind_int64(1, now),
      stmt->bind_text(2, to_string(request.reason)),
      who,
      stmt->bind_int64(4, request.seq),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind terminating mark", result.error());
    }
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

/// @brief The history outcome a stored `terminate_reason` names.
auto outcome_for(std::string_view reason) -> std::optional<history_outcome> {
  if (reason == "timeout") {
    return history_outcome::timeout;
  }
  if (reason == "cancelled") {
    return history_outcome::cancelled;
  }
  return std::nullopt;
}

} // namespace

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
  return [](std::int64_t pgid, int sig) { return identity::signal_group(pgid, sig); };
}

auto signal_child_group(const entry& e, stop_signal sig, std::string_view host_id, const process_probe& probe,
                        const group_signaller& signaller) -> signal_attempt {
  signal_attempt attempt{.seq = e.seq, .signal = sig, .outcome = signal_outcome::no_group, .error = std::nullopt};
  if (!same_host(e, host_id)) {
    attempt.outcome = signal_outcome::other_host;
    return attempt;
  }
  if (!verifiable_group(e)) {
    return attempt;
  }
  auto verdict = judge_child_group(e, probe);
  if (!verdict) {
    attempt.outcome = signal_outcome::failed;
    attempt.error   = verdict.error();
    return attempt;
  }
  switch (*verdict) {
  case group_verdict::reused:
    attempt.outcome = signal_outcome::reused;
    return attempt;
  case group_verdict::empty:
    attempt.outcome = signal_outcome::group_empty;
    return attempt;
  case group_verdict::not_checked:
    return attempt;
  case group_verdict::has_members:
    break;
  }
  if (auto sent = signaller(*e.child_pgid, signal_number(sig)); !sent) {
    attempt.outcome = signal_outcome::failed;
    attempt.error   = sent.error();
    return attempt;
  }
  attempt.outcome = signal_outcome::sent;
  return attempt;
}

auto begin_terminate(db::connection& conn, const begin_terminate_request& request, process::identity::clock& clock,
                     const process_probe& probe, const group_signaller& signaller) -> std::expected<begin_result, queue_error> {
  constexpr std::string_view op = "begin terminate";
  if (request.reason == stop_reason::cancelled && !request.cancelled_by) {
    return invalid(op, "reason cancelled requires a canceller");
  }
  if (request.reason == stop_reason::timeout && request.cancelled_by) {
    return invalid(op, "reason timeout takes no canceller");
  }
  if (conn.in_transaction()) {
    // The marker must commit, and the lock be released, before any signal.
    return invalid(op, "the connection is already in a transaction");
  }

  begin_result result;
  {
    auto txn = conn.begin_transaction(db::lock_mode::immediate);
    if (!txn) {
      return sql_failure("begin terminating mark", txn.error());
    }
    // Decision 1203: the monotonic clock is read once the write lock is held.
    auto const now = clock.monotonic_ms();
    if (!now) {
      return clock_failure(op);
    }
    auto found = find(conn, request.seq);
    if (!found) {
      return std::unexpected(std::move(found.error()));
    }
    if (!found->has_value()) {
      result.status = begin_status::missing;
      return result; // the transaction rolls back; it wrote nothing
    }
    entry current = std::move(**found);
    if (current.terminating_since_mono) {
      result.status = begin_status::already_terminating;
      result.stored = std::move(current);
      return result;
    }
    if (current.state != entry_state::running) {
      result.status = begin_status::not_running;
      result.stored = std::move(current);
      return result;
    }
    auto marked = mark(conn, request, *now);
    if (!marked) {
      return std::unexpected(std::move(marked.error()));
    }
    if (!*marked) {
      // Unreachable while the write lock is held: the entry was read as
      // running and unmarked in this transaction.
      result.status = begin_status::already_terminating;
      result.stored = std::move(current);
      return result;
    }
    if (auto committed = txn->commit(); !committed) {
      return sql_failure("commit terminating mark", committed.error());
    }
    current.terminating_since_mono = *now;
    current.terminate_reason       = std::string(to_string(request.reason));
    current.cancelled_by =
        request.cancelled_by ? std::optional<std::string>(encode_canceller(*request.cancelled_by)) : std::nullopt;
    result.status = begin_status::marked;
    result.stored = std::move(current);
  }

  // The marker is committed and the write lock released: now signal.
  result.sigterm = signal_child_group(*result.stored, stop_signal::term, request.host_id, probe, signaller);
  return result;
}

auto advance_terminations(db::connection& conn, const advance_request& request, process::identity::clock& clock,
                          const process_probe& probe, const group_signaller& signaller)
    -> std::expected<advance_result, queue_error> {
  constexpr std::string_view op = "advance terminations";
  if (request.grace_ms < 0) {
    return invalid(op, std::format("grace period must not be negative, got {} ms", request.grace_ms));
  }
  if (conn.in_transaction()) {
    // Signals are never sent while a transaction is open on the store.
    return invalid(op, "the connection is already in a transaction");
  }
  auto const now = clock.monotonic_ms();
  if (!now) {
    return clock_failure(op);
  }

  // Read in autocommit: no lock is held while groups are judged or
  // signalled. Each end is its own short transaction inside `end_entry`.
  auto entries = list(conn);
  if (!entries) {
    return std::unexpected(std::move(entries.error()));
  }

  advance_result result{.now_mono = *now};
  for (auto const& e : *entries) {
    if (request.seq && e.seq != *request.seq) {
      continue;
    }
    if (!e.terminating_since_mono || !e.terminate_reason || e.state != entry_state::running) {
      continue;
    }
    if (!same_host(e, request.host_id) || !verifiable_group(e)) {
      continue;
    }
    auto const outcome = outcome_for(*e.terminate_reason);
    if (!outcome) {
      continue;
    }

    auto verdict = judge_child_group(e, probe);
    if (!verdict) {
      result.failures.push_back(
          signal_attempt{.seq = e.seq, .signal = stop_signal::kill, .outcome = signal_outcome::failed, .error = verdict.error()});
      continue;
    }

    if (*verdict == group_verdict::empty || *verdict == group_verdict::reused) {
      // An empty group stays empty, and a reused id counts as empty, so the
      // judgement made outside the lock still holds when the end commits.
      auto ended = end_entry(conn, e.seq, end_request{.outcome = *outcome, .ended_at = clock.wall_ms()});
      if (!ended) {
        return std::unexpected(std::move(ended.error()));
      }
      if (*ended == end_result::ended) {
        result.ended.push_back(ended_termination{.seq = e.seq, .outcome = *outcome});
      }
      continue;
    }

    // The group has members. Past the grace period, kill it; the entry is
    // removed by a later call once the group is empty.
    // Unsigned distance, so no pair of clock values overflows.
    auto const since   = *e.terminating_since_mono;
    bool const overdue = *now > since && static_cast<std::uint64_t>(*now) - static_cast<std::uint64_t>(since) >
                                             static_cast<std::uint64_t>(request.grace_ms);
    if (overdue) {
      result.kills.push_back(signal_child_group(e, stop_signal::kill, request.host_id, probe, signaller));
    }
  }
  return result;
}

auto poll_and_stop(db::connection& conn, const poll_stop_request& request, process::identity::clock& clock,
                   const process_probe& probe, const group_signaller& signaller) -> std::expected<poll_stop_result, queue_error> {
  if (request.grace_ms < 0) {
    return invalid("poll and stop", std::format("grace period must not be negative, got {} ms", request.grace_ms));
  }
  auto polled = poll(conn, request.poll, clock, probe);
  if (!polled) {
    return std::unexpected(std::move(polled.error()));
  }
  poll_stop_result result{.poll = std::move(*polled)};
  if (result.poll.status == poll_status::skipped) {
    return result;
  }

  // The poll has committed: signal the entries it marked.
  for (auto const& marked : result.poll.terminating) {
    result.sigterms.push_back(signal_child_group(marked, stop_signal::term, request.poll.host_id, probe, signaller));
  }

  auto advanced = advance_terminations(
      conn, advance_request{.host_id = request.poll.host_id, .grace_ms = request.grace_ms, .seq = std::nullopt}, clock, probe,
      signaller);
  if (advanced) {
    result.advanced = std::move(*advanced);
  } else {
    result.advance_error = std::move(advanced.error());
  }
  return result;
}

} // namespace planar::engine::hostqueue
