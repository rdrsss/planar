/// @file history.cpp
/// @brief Implementation of `planar.engine.hostqueue.history` (plan 1080,
/// task hq-history). See history.cppm for the contract.

module planar.engine.hostqueue.history;

import std;
import planar.db;
import planar.json_dom;
import planar.json_text;
import planar.engine.hostqueue.queue;

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
auto invalid(std::string message) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::invalid_request,
      .sqlite_code = 0,
      .message     = std::format("hostqueue: {}", message),
  });
}

/// @brief Binds `value` as text, or SQL NULL when absent.
auto bind_optional_text(db::statement& stmt, int index, const std::optional<std::string>& value)
    -> std::expected<void, db::db_error> {
  return value ? stmt.bind_text(index, *value) : stmt.bind_null(index);
}

/// @brief Binds `value` as an integer, or SQL NULL when absent.
auto bind_optional_int(db::statement& stmt, int index, const std::optional<std::int64_t>& value)
    -> std::expected<void, db::db_error> {
  return value ? stmt.bind_int64(index, *value) : stmt.bind_null(index);
}

/// @brief Reads column `index` as text, or `std::nullopt` when SQL NULL.
auto optional_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

/// @brief Reads column `index` as an integer, or `std::nullopt` when SQL NULL.
auto optional_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

/// @brief Every outcome with its stored text, in CHECK order.
constexpr std::array<std::pair<history_outcome, std::string_view>, 7> k_outcomes{{
    {history_outcome::exited, "exited"},
    {history_outcome::signaled, "signaled"},
    {history_outcome::timeout, "timeout"},
    {history_outcome::cancelled, "cancelled"},
    {history_outcome::wait_timeout, "wait_timeout"},
    {history_outcome::not_started, "not_started"},
    {history_outcome::abandoned, "abandoned"},
}};

/// @brief Checks that the request's optional fields fit its outcome (see
/// `end_request`). The canceller of a `cancelled` request may still come
/// from the entry, so its presence is checked by the caller.
auto check_request(const end_request& request) -> std::expected<void, queue_error> {
  auto const outcome = request.outcome;
  auto const name    = to_string(outcome);
  switch (outcome) {
  case history_outcome::exited:
    if (!request.exit_code) {
      return invalid("outcome exited requires an exit code");
    }
    break;
  case history_outcome::not_started:
    if (!request.exit_code || (*request.exit_code != 126 && *request.exit_code != 127)) {
      return invalid("outcome not_started requires exit code 126 or 127");
    }
    break;
  default:
    if (request.exit_code) {
      return invalid(std::format("outcome {} records no exit code", name));
    }
    break;
  }
  if (outcome == history_outcome::signaled) {
    if (!request.signal) {
      return invalid("outcome signaled requires a signal");
    }
  } else if (request.signal) {
    return invalid(std::format("outcome {} records no signal", name));
  }
  if (outcome != history_outcome::cancelled && request.cancelled_by) {
    return invalid(std::format("outcome {} records no canceller", name));
  }
  return {};
}

/// @brief The column list every history read shares, in the order
/// `read_history` consumes it.
constexpr std::string_view k_history_columns = "seq, outcome, exit_code, signal, successor_seq, cancelled_by, nested, "
                                               "parent_seq, cwd, argv, label, vendor, role, log_path, enqueued_at, "
                                               "started_at, ended_at, waited_ms, ran_ms";

/// @brief Materialises the current row of a `k_history_columns` statement.
auto read_history(const db::statement& stmt) -> std::expected<history_row, queue_error> {
  history_row out;
  out.seq           = stmt.column_int64(0);
  auto const stored = stmt.column_text(1);
  auto const parsed = parse_history_outcome(stored);
  if (!parsed) {
    // The CHECK constraint makes this unreachable for a store at head.
    return invalid(std::format("stored history outcome is unknown: {}", stored));
  }
  out.outcome       = *parsed;
  out.exit_code     = optional_int(stmt, 2);
  out.signal        = optional_int(stmt, 3);
  out.successor_seq = optional_int(stmt, 4);
  if (auto const text = optional_text(stmt, 5)) {
    auto who = decode_canceller(*text);
    if (!who) {
      return std::unexpected(std::move(who.error()));
    }
    out.cancelled_by = std::move(*who);
  }
  out.nested     = stmt.column_int64(6) != 0;
  out.parent_seq = optional_int(stmt, 7);
  out.cwd        = stmt.column_text(8);
  auto argv      = decode_argv(stmt.column_text(9));
  if (!argv) {
    return std::unexpected(std::move(argv.error()));
  }
  out.argv        = std::move(*argv);
  out.label       = optional_text(stmt, 10);
  out.vendor      = optional_text(stmt, 11);
  out.role        = optional_text(stmt, 12);
  out.log_path    = optional_text(stmt, 13);
  out.enqueued_at = stmt.column_int64(14);
  out.started_at  = optional_int(stmt, 15);
  out.ended_at    = stmt.column_int64(16);
  out.waited_ms   = stmt.column_int64(17);
  out.ran_ms      = optional_int(stmt, 18);
  return out;
}

/// @brief The entry columns `end_entry` copies into the history row, as the
/// delete returned them. `argv` stays as stored text: it is copied, not
/// reinterpreted.
struct removed_entry {
  std::optional<std::int64_t> parent_seq;
  std::optional<std::string>  cancelled_by;
  std::string                 cwd;
  std::string                 argv;
  std::optional<std::string>  label;
  std::optional<std::string>  vendor;
  std::optional<std::string>  role;
  std::optional<std::string>  log_path;
  std::int64_t                enqueued_at = 0;
  std::optional<std::int64_t> started_at;
};

/// @brief Deletes entry `seq` and returns the columns the history row needs,
/// or `std::nullopt` when the delete removed nothing.
auto delete_entry(db::connection& conn, std::int64_t seq) -> std::expected<std::optional<removed_entry>, queue_error> {
  auto stmt = conn.prepare("delete from queue_entries where seq = ? returning parent_seq, cancelled_by, cwd, argv, label, "
                           "vendor, role, log_path, enqueued_at, started_at");
  if (!stmt) {
    return sql_failure("prepare entry delete", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, seq); !bound) {
    return sql_failure("bind entry delete", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("delete queue entry", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return std::optional<removed_entry>{};
  }
  removed_entry out{
      .parent_seq   = optional_int(*stmt, 0),
      .cancelled_by = optional_text(*stmt, 1),
      .cwd          = stmt->column_text(2),
      .argv         = stmt->column_text(3),
      .label        = optional_text(*stmt, 4),
      .vendor       = optional_text(*stmt, 5),
      .role         = optional_text(*stmt, 6),
      .log_path     = optional_text(*stmt, 7),
      .enqueued_at  = stmt->column_int64(8),
      .started_at   = optional_int(*stmt, 9),
  };
  // A `returning` delete is not finished until it reports done.
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish queue entry delete", done.error());
  }
  return std::optional<removed_entry>{std::move(out)};
}

/// @brief `later - earlier`, never below zero: wall-clock durations are for
/// display, and a clock step must not produce a negative one.
auto elapsed(std::int64_t earlier, std::int64_t later) -> std::int64_t {
  return later > earlier ? later - earlier : 0;
}

/// @brief Inserts the history row for `removed`.
auto insert_history(db::connection& conn, std::int64_t seq, const end_request& request, const removed_entry& removed,
                    const std::optional<std::string>& cancelled_by) -> std::expected<void, queue_error> {
  auto stmt = conn.prepare("insert into queue_history (seq, outcome, exit_code, signal, successor_seq, cancelled_by, nested, "
                           "parent_seq, cwd, argv, label, vendor, role, log_path, enqueued_at, started_at, ended_at, "
                           "waited_ms, ran_ms) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  if (!stmt) {
    return sql_failure("prepare history insert", stmt.error());
  }
  auto const waited = elapsed(removed.enqueued_at, removed.started_at.value_or(request.ended_at));
  auto const ran    = removed.started_at ? std::optional<std::int64_t>{elapsed(*removed.started_at, request.ended_at)}
                                         : std::optional<std::int64_t>{};

  std::array<std::expected<void, db::db_error>, 19> const bound{{
      stmt->bind_int64(1, seq),
      stmt->bind_text(2, to_string(request.outcome)),
      bind_optional_int(*stmt, 3, request.exit_code),
      bind_optional_int(*stmt, 4, request.signal),
      bind_optional_int(*stmt, 5, request.successor_seq),
      bind_optional_text(*stmt, 6, cancelled_by),
      stmt->bind_int64(7, removed.parent_seq ? 1 : 0),
      bind_optional_int(*stmt, 8, removed.parent_seq),
      stmt->bind_text(9, removed.cwd),
      stmt->bind_text(10, removed.argv),
      bind_optional_text(*stmt, 11, removed.label),
      bind_optional_text(*stmt, 12, removed.vendor),
      bind_optional_text(*stmt, 13, removed.role),
      bind_optional_text(*stmt, 14, removed.log_path),
      stmt->bind_int64(15, removed.enqueued_at),
      bind_optional_int(*stmt, 16, removed.started_at),
      stmt->bind_int64(17, request.ended_at),
      stmt->bind_int64(18, waited),
      bind_optional_int(*stmt, 19, ran),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind history insert", result.error());
    }
  }
  if (auto stepped = stmt->step(); !stepped) {
    return sql_failure("insert history row", stepped.error());
  }
  return {};
}

} // namespace

auto to_string(history_outcome outcome) -> std::string_view {
  for (auto const& [value, text] : k_outcomes) {
    if (value == outcome) {
      return text;
    }
  }
  return "exited"; // unreachable: k_outcomes names every enumerator
}

auto parse_history_outcome(std::string_view text) -> std::optional<history_outcome> {
  for (auto const& [value, name] : k_outcomes) {
    if (name == text) {
      return value;
    }
  }
  return std::nullopt;
}

auto encode_canceller(const canceller& who) -> std::string {
  std::string out = "{\"vendor\":";
  if (who.vendor) {
    json_text::append_json_string(out, *who.vendor);
  } else {
    out += "null";
  }
  out += ",\"role\":";
  if (who.role) {
    json_text::append_json_string(out, *who.role);
  } else {
    out += "null";
  }
  out += std::format(",\"pid\":{}}}", who.pid);
  return out;
}

auto decode_canceller(std::string_view text) -> std::expected<canceller, queue_error> {
  auto const malformed = [&] {
    return std::unexpected(queue_error{
        .kind        = queue_error_kind::malformed_canceller,
        .sqlite_code = 0,
        .message     = std::format("hostqueue: stored canceller is not a {{vendor, role, pid}} object: {}", text),
    });
  };
  auto parsed = json_dom::parse_json(text);
  if (!parsed || parsed->kind != json_dom::json_kind::object) {
    return malformed();
  }
  auto const* pid = parsed->find("pid");
  if (pid == nullptr || pid->kind != json_dom::json_kind::integer) {
    return malformed();
  }
  canceller out{.pid = pid->integer};
  for (auto const& [key, slot] : {std::pair{"vendor", &out.vendor}, std::pair{"role", &out.role}}) {
    auto const* member = parsed->find(key);
    if (member == nullptr || member->kind == json_dom::json_kind::null_) {
      continue;
    }
    if (member->kind != json_dom::json_kind::string) {
      return malformed();
    }
    *slot = member->string;
  }
  return out;
}

auto end_entry(db::connection& conn, std::int64_t seq, const end_request& request) -> std::expected<end_result, queue_error> {
  if (auto checked = check_request(request); !checked) {
    return std::unexpected(std::move(checked.error()));
  }
  // IMMEDIATE takes the write lock at BEGIN, so of two processes ending the
  // same entry the second waits for the first's commit, then finds nothing
  // to delete (tech spec 647: "a process that finds the entry already
  // deleted writes nothing").
  auto txn = conn.begin_transaction(db::lock_mode::immediate);
  if (!txn) {
    return sql_failure("begin end entry", txn.error());
  }
  auto removed = delete_entry(conn, seq);
  if (!removed) {
    return std::unexpected(std::move(removed.error()));
  }
  if (!removed->has_value()) {
    return end_result::already_gone; // the transaction rolls back; it changed nothing
  }
  auto const& entry = **removed;

  std::optional<std::string> cancelled_by;
  if (request.outcome == history_outcome::cancelled) {
    if (request.cancelled_by) {
      cancelled_by = encode_canceller(*request.cancelled_by);
    } else if (entry.cancelled_by) {
      auto who = decode_canceller(*entry.cancelled_by);
      if (!who) {
        return std::unexpected(std::move(who.error()));
      }
      cancelled_by = encode_canceller(*who);
    } else {
      return invalid(std::format("outcome cancelled requires a canceller, and entry {} records none", seq));
    }
  }

  if (auto inserted = insert_history(conn, seq, request, entry, cancelled_by); !inserted) {
    return std::unexpected(std::move(inserted.error()));
  }
  if (auto committed = txn->commit(); !committed) {
    return sql_failure("commit end entry", committed.error());
  }
  return end_result::ended;
}

auto record_successor(db::connection& conn, std::int64_t seq, std::int64_t successor_seq)
    -> std::expected<successor_result, queue_error> {
  auto stmt = conn.prepare("update queue_history set successor_seq = ? where seq = ? returning seq");
  if (!stmt) {
    return sql_failure("prepare successor update", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, successor_seq); !bound) {
    return sql_failure("bind successor update", bound.error());
  }
  if (auto bound = stmt->bind_int64(2, seq); !bound) {
    return sql_failure("bind successor update", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("record successor", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return successor_result::no_such_entry;
  }
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish successor update", done.error());
  }
  return successor_result::recorded;
}

auto rejoin(db::connection& conn, std::int64_t old_seq, const enqueue_request& request)
    -> std::expected<rejoin_result, queue_error> {
  auto txn = conn.begin_transaction(db::lock_mode::immediate);
  if (!txn) {
    return sql_failure("begin rejoin", txn.error());
  }
  auto row = find_history(conn, old_seq);
  if (!row) {
    return std::unexpected(std::move(row.error()));
  }
  if (!row->has_value()) {
    return rejoin_result{.status = rejoin_status::no_history};
  }
  if ((*row)->outcome != history_outcome::abandoned) {
    return rejoin_result{.status = rejoin_status::not_abandoned, .seq = 0, .outcome = (*row)->outcome};
  }
  auto inserted = enqueue(conn, request);
  if (!inserted) {
    return std::unexpected(std::move(inserted.error()));
  }
  auto recorded = record_successor(conn, old_seq, *inserted);
  if (!recorded) {
    return std::unexpected(std::move(recorded.error()));
  }
  if (auto committed = txn->commit(); !committed) {
    return sql_failure("commit rejoin", committed.error());
  }
  return rejoin_result{.status = rejoin_status::rejoined, .seq = *inserted, .outcome = std::nullopt};
}

auto find_history(db::connection& conn, std::int64_t seq) -> std::expected<std::optional<history_row>, queue_error> {
  auto stmt = conn.prepare(std::format("select {} from queue_history where seq = ?", k_history_columns));
  if (!stmt) {
    return sql_failure("prepare find history", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, seq); !bound) {
    return sql_failure("bind find history", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("read history row", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return std::optional<history_row>{};
  }
  auto row = read_history(*stmt);
  if (!row) {
    return std::unexpected(std::move(row.error()));
  }
  return std::optional<history_row>{std::move(*row)};
}

auto list_history(db::connection& conn, std::optional<std::int64_t> ended_since)
    -> std::expected<std::vector<history_row>, queue_error> {
  auto stmt = conn.prepare(std::format("select {} from queue_history where (?1 is null or ended_at >= ?1) "
                                       "order by ended_at, seq",
                                       k_history_columns));
  if (!stmt) {
    return sql_failure("prepare list history", stmt.error());
  }
  if (auto bound = bind_optional_int(*stmt, 1, ended_since); !bound) {
    return sql_failure("bind list history", bound.error());
  }
  std::vector<history_row> out;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return sql_failure("read history rows", stepped.error());
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto row = read_history(*stmt);
    if (!row) {
      return std::unexpected(std::move(row.error()));
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto delete_expired_history(db::connection& conn, std::int64_t retention_days, std::int64_t now)
    -> std::expected<expired_history, queue_error> {
  if (retention_days < 0) {
    return invalid(std::format("history retention must not be negative, got {} days", retention_days));
  }
  // Saturate rather than overflow for an absurd retention: nothing is older.
  auto const limit = std::numeric_limits<std::int64_t>::max() / k_ms_per_day;
  auto const span  = retention_days > limit ? std::numeric_limits<std::int64_t>::max() : retention_days * k_ms_per_day;
  auto const cutoff =
      now >= std::numeric_limits<std::int64_t>::min() + span ? now - span : std::numeric_limits<std::int64_t>::min();

  auto stmt = conn.prepare("delete from queue_history where ended_at < ? returning log_path");
  if (!stmt) {
    return sql_failure("prepare history prune", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, cutoff); !bound) {
    return sql_failure("bind history prune", bound.error());
  }
  expired_history out;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return sql_failure("prune history", stepped.error());
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    ++out.rows_deleted;
    if (auto path = optional_text(*stmt, 0)) {
      out.log_paths.push_back(std::move(*path));
    }
  }
  return out;
}

auto remove_log_files(std::span<std::string const> paths) -> std::vector<log_removal_failure> {
  std::vector<log_removal_failure> failures;
  for (auto const& path : paths) {
    std::error_code ec;
    // `remove` returns false without an error for a path that does not
    // exist: an already-removed log is removed.
    std::filesystem::remove(path, ec);
    if (ec) {
      failures.push_back(log_removal_failure{.path = path, .message = ec.message()});
    }
  }
  return failures;
}

} // namespace planar::engine::hostqueue
