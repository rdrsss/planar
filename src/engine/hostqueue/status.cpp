/// @file status.cpp
/// @brief Implementation of `planar.engine.hostqueue.status` (plan 1080, task
/// hq-queue-status). See status.cppm for the contract.

module planar.engine.hostqueue.status;

import std;
import planar.db;
import planar.process.identity;
import planar.engine.hostqueue.queue;
import planar.engine.hostqueue.history;
import planar.engine.hostqueue.liveness;

namespace planar::engine::hostqueue {

namespace {

auto store_failure(const queue_error& error) -> std::unexpected<status_error> {
  return std::unexpected(status_error{.kind = status_error_kind::store, .message = error.message});
}

/// @brief A duration between two wall-clock readings, never negative: the wall
/// clock may step backwards, and a negative wait is not a wait.
auto span_ms(std::int64_t from, std::int64_t to) -> std::int64_t {
  return std::max<std::int64_t>(0, to - from);
}

/// @brief The description of an entry still in the queue.
auto describe_entry(db::connection& conn, std::int64_t asked, const entry& e, const status_request& request)
    -> std::expected<queue_status, status_error> {
  auto settings = request.settings();
  if (!settings) {
    return std::unexpected(status_error{.kind = status_error_kind::settings, .message = settings.error()});
  }

  queue_status status;
  status.seq         = asked;
  status.state       = e.state == entry_state::waiting ? status_state::waiting : status_state::running;
  status.nested      = e.parent_seq.has_value();
  status.parent_seq  = e.parent_seq;
  status.cwd         = e.cwd;
  status.argv        = e.argv;
  status.label       = e.label;
  status.vendor      = e.vendor;
  status.role        = e.role;
  status.log_path    = e.log_path;
  status.enqueued_at = e.enqueued_at;
  status.started_at  = e.started_at;
  status.terminating = e.terminate_reason;
  status.slots       = settings->slots;
  status.grace_ms    = settings->grace_ms;

  if (e.cancelled_by.has_value()) {
    auto who = decode_canceller(*e.cancelled_by);
    if (!who) {
      return store_failure(who.error());
    }
    status.cancelled_by = *who;
  }

  // Liveness by the rules a poll applies, judged and left alone. A probe that
  // fails leaves the answer empty rather than guessing.
  auto const verdict = judge_liveness(
      e, liveness_context{.host_id = request.host_id, .now_mono = request.now_mono, .stale_after_ms = settings->stale_after_ms},
      request.probe);
  if (verdict) {
    status.live = verdict->live;
  }

  if (e.state == entry_state::waiting) {
    // The place among the waiting entries, arrival order: running entries hold
    // slots and do not count.
    auto entries = list(conn);
    if (!entries) {
      return store_failure(entries.error());
    }
    std::int64_t place = 0;
    for (auto const& other : *entries) {
      if (other.state == entry_state::waiting && other.seq <= e.seq) {
        ++place;
      }
    }
    status.position  = place;
    status.waited_ms = span_ms(e.enqueued_at, request.now_wall);
  } else if (e.started_at.has_value()) {
    status.waited_ms = span_ms(e.enqueued_at, *e.started_at);
    status.ran_ms    = span_ms(*e.started_at, request.now_wall);
  }
  return status;
}

/// @brief The description of an entry that has ended.
auto describe_history(std::int64_t asked, const history_row& row) -> queue_status {
  queue_status status;
  status.seq          = asked;
  status.state        = status_state::ended;
  status.outcome      = row.outcome;
  status.exit_code    = row.exit_code;
  status.signal       = row.signal;
  status.cancelled_by = row.cancelled_by;
  status.nested       = row.nested;
  status.parent_seq   = row.parent_seq;
  status.cwd          = row.cwd;
  status.argv         = row.argv;
  status.label        = row.label;
  status.vendor       = row.vendor;
  status.role         = row.role;
  status.log_path     = row.log_path;
  status.enqueued_at  = row.enqueued_at;
  status.started_at   = row.started_at;
  status.ended_at     = row.ended_at;
  status.waited_ms    = row.waited_ms;
  status.ran_ms       = row.ran_ms;
  return status;
}

} // namespace

auto to_string(status_state state) -> std::string_view {
  switch (state) {
  case status_state::waiting:
    return "waiting";
  case status_state::running:
    return "running";
  case status_state::ended:
    return "ended";
  }
  return "ended";
}

auto query_status(db::connection& conn, std::int64_t seq, const status_request& request)
    -> std::expected<std::optional<queue_status>, status_error> {
  // Follow the successor chain from the number asked for. `visited` bounds it,
  // so a store that names a cycle ends the chain instead of looping.
  std::set<std::int64_t>      visited{seq};
  std::optional<history_row>  last;
  std::optional<std::int64_t> superseded_by;
  std::int64_t                current = seq;

  for (;;) {
    auto in_queue = find(conn, current);
    if (!in_queue) {
      return store_failure(in_queue.error());
    }
    if (in_queue->has_value()) {
      auto described = describe_entry(conn, seq, **in_queue, request);
      if (!described) {
        return std::unexpected(std::move(described.error()));
      }
      described->superseded_by = superseded_by;
      return std::optional<queue_status>{std::move(*described)};
    }

    auto ended = find_history(conn, current);
    if (!ended) {
      return store_failure(ended.error());
    }
    if (!ended->has_value()) {
      break; // Never issued, or pruned: with nothing before it, nothing is known.
    }
    last = **ended;
    if (!last->successor_seq.has_value() || visited.contains(*last->successor_seq)) {
      auto described          = describe_history(seq, *last);
      described.superseded_by = superseded_by;
      return std::optional<queue_status>{std::move(described)};
    }
    superseded_by = *last->successor_seq;
    visited.insert(*last->successor_seq);
    current = *last->successor_seq;
  }

  if (!last.has_value()) {
    return std::optional<queue_status>{};
  }
  // The successor has neither an entry nor a history row: report the last row
  // that exists, and the number it named.
  auto described          = describe_history(seq, *last);
  described.superseded_by = superseded_by;
  return std::optional<queue_status>{std::move(described)};
}

} // namespace planar::engine::hostqueue
