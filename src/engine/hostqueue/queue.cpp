/// @file queue.cpp
/// @brief Implementation of `planar.engine.hostqueue.queue` (plan 1080, task
/// hq-enqueue; the pruning enqueue, task hq-history). See queue.cppm for the
/// contract.

module planar.engine.hostqueue.queue;

import std;
import planar.db;
import planar.engine.hostqueue.history;
import planar.json_dom;
import planar.json_text;

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

/// @brief The column list every read shares, in the order `read_entry`
/// consumes it.
constexpr std::string_view k_entry_columns = "seq, state, host_id, pid, pid_started, child_pgid, child_started, parent_seq, "
                                             "terminating_since_mono, terminate_reason, cancelled_by, cwd, argv, label, "
                                             "vendor, role, claim_token, log_path, enqueued_at, started_at, "
                                             "refreshed_mono, deadline_mono, wait_deadline_mono";

/// @brief Materialises the current row of a `k_entry_columns` statement.
auto read_entry(const db::statement& stmt) -> std::expected<entry, queue_error> {
  entry out;
  out.seq                    = stmt.column_int64(0);
  auto const state           = stmt.column_text(1);
  out.state                  = state == "running" ? entry_state::running : entry_state::waiting;
  out.host_id                = stmt.column_text(2);
  out.pid                    = stmt.column_int64(3);
  out.pid_started            = stmt.column_int64(4);
  out.child_pgid             = optional_int(stmt, 5);
  out.child_started          = optional_int(stmt, 6);
  out.parent_seq             = optional_int(stmt, 7);
  out.terminating_since_mono = optional_int(stmt, 8);
  out.terminate_reason       = optional_text(stmt, 9);
  out.cancelled_by           = optional_text(stmt, 10);
  out.cwd                    = stmt.column_text(11);
  auto argv                  = decode_argv(stmt.column_text(12));
  if (!argv) {
    return std::unexpected(std::move(argv.error()));
  }
  out.argv               = std::move(*argv);
  out.label              = optional_text(stmt, 13);
  out.vendor             = optional_text(stmt, 14);
  out.role               = optional_text(stmt, 15);
  out.claim_token        = optional_text(stmt, 16);
  out.log_path           = optional_text(stmt, 17);
  out.enqueued_at        = stmt.column_int64(18);
  out.started_at         = optional_int(stmt, 19);
  out.refreshed_mono     = stmt.column_int64(20);
  out.deadline_mono      = optional_int(stmt, 21);
  out.wait_deadline_mono = optional_int(stmt, 22);
  return out;
}

} // namespace

auto to_string(entry_state state) -> std::string_view {
  return state == entry_state::running ? "running" : "waiting";
}

auto encode_argv(std::span<std::string const> argv) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    json_text::append_json_string(out, argv[i]);
  }
  out += ']';
  return out;
}

auto decode_argv(std::string_view text) -> std::expected<std::vector<std::string>, queue_error> {
  auto const malformed = [&] {
    return std::unexpected(queue_error{
        .kind        = queue_error_kind::malformed_argv,
        .sqlite_code = 0,
        .message     = std::format("hostqueue: stored argv is not a JSON array of strings: {}", text),
    });
  };
  auto parsed = json_dom::parse_json(text);
  if (!parsed || parsed->kind != json_dom::json_kind::array) {
    return malformed();
  }
  std::vector<std::string> argv;
  argv.reserve(parsed->array.size());
  for (auto& element : parsed->array) {
    if (element.kind != json_dom::json_kind::string) {
      return malformed();
    }
    argv.push_back(std::move(element.string));
  }
  return argv;
}

auto enqueue(db::connection& conn, const enqueue_request& request) -> std::expected<std::int64_t, queue_error> {
  auto stmt = conn.prepare("insert into queue_entries (state, host_id, pid, pid_started, parent_seq, cwd, argv, label, "
                           "vendor, role, claim_token, log_path, enqueued_at, refreshed_mono, wait_deadline_mono) "
                           "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) returning seq");
  if (!stmt) {
    return sql_failure("prepare enqueue", stmt.error());
  }
  // A nested run is inserted running: it never takes a slot or a turn
  // (roadmap M1, task hq-nested-entry owns the rest of that behaviour).
  auto const state = request.parent_seq ? entry_state::running : entry_state::waiting;

  std::array<std::expected<void, db::db_error>, 15> const bound{{
      stmt->bind_text(1, to_string(state)),
      stmt->bind_text(2, request.host_id),
      stmt->bind_int64(3, request.pid),
      stmt->bind_int64(4, request.pid_started),
      bind_optional_int(*stmt, 5, request.parent_seq),
      stmt->bind_text(6, request.cwd),
      stmt->bind_text(7, encode_argv(request.argv)),
      bind_optional_text(*stmt, 8, request.label),
      bind_optional_text(*stmt, 9, request.vendor),
      bind_optional_text(*stmt, 10, request.role),
      bind_optional_text(*stmt, 11, request.claim_token),
      bind_optional_text(*stmt, 12, request.log_path),
      stmt->bind_int64(13, request.enqueued_at),
      stmt->bind_int64(14, request.refreshed_mono),
      bind_optional_int(*stmt, 15, request.wait_deadline_mono),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind enqueue", result.error());
    }
  }

  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("insert queue entry", stepped.error());
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(queue_error{
        .kind        = queue_error_kind::query_failed,
        .sqlite_code = 0,
        .message     = "hostqueue: insert queue entry returned no sequence number",
    });
  }
  auto const seq = stmt->column_int64(0);
  // Drain the statement so the insert is finished before the handle is
  // finalised; a `returning` insert is not complete until it reports done.
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish queue entry insert", done.error());
  }
  return seq;
}

auto enqueue(db::connection& conn, const enqueue_request& request, std::int64_t history_days)
    -> std::expected<enqueued, queue_error> {
  auto txn = conn.begin_transaction(db::lock_mode::immediate);
  if (!txn) {
    return sql_failure("begin enqueue", txn.error());
  }
  auto expired = delete_expired_history(conn, history_days, request.enqueued_at);
  if (!expired) {
    return std::unexpected(std::move(expired.error()));
  }
  auto seq = enqueue(conn, request);
  if (!seq) {
    return std::unexpected(std::move(seq.error()));
  }
  if (auto committed = txn->commit(); !committed) {
    return sql_failure("commit enqueue", committed.error());
  }
  // Files go only after the rows are committed away: a rolled-back prune
  // must never leave a kept row whose log is gone.
  return enqueued{
      .seq    = *seq,
      .pruned = prune_report{.rows_deleted = expired->rows_deleted, .log_failures = remove_log_files(expired->log_paths)},
  };
}

auto record_child(db::connection& conn, std::int64_t seq, std::int64_t child_pgid, std::int64_t child_started)
    -> std::expected<bool, queue_error> {
  auto stmt = conn.prepare("update queue_entries set child_pgid = ?, child_started = ? "
                           "where seq = ? and state = 'running' returning seq");
  if (!stmt) {
    return sql_failure("prepare record child", stmt.error());
  }
  std::array<std::expected<void, db::db_error>, 3> const bound{{
      stmt->bind_int64(1, child_pgid),
      stmt->bind_int64(2, child_started),
      stmt->bind_int64(3, seq),
  }};
  for (auto const& result : bound) {
    if (!result) {
      return sql_failure("bind record child", result.error());
    }
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("record child group", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return false;
  }
  if (auto done = stmt->step(); !done) {
    return sql_failure("finish record child", done.error());
  }
  return true;
}

auto find(db::connection& conn, std::int64_t seq) -> std::expected<std::optional<entry>, queue_error> {
  auto stmt = conn.prepare(std::format("select {} from queue_entries where seq = ?", k_entry_columns));
  if (!stmt) {
    return sql_failure("prepare find", stmt.error());
  }
  if (auto bound = stmt->bind_int64(1, seq); !bound) {
    return sql_failure("bind find", bound.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return sql_failure("read queue entry", stepped.error());
  }
  if (*stepped == db::step_result::done) {
    return std::optional<entry>{};
  }
  auto row = read_entry(*stmt);
  if (!row) {
    return std::unexpected(std::move(row.error()));
  }
  return std::optional<entry>{std::move(*row)};
}

auto list(db::connection& conn) -> std::expected<std::vector<entry>, queue_error> {
  auto stmt = conn.prepare(std::format("select {} from queue_entries order by seq", k_entry_columns));
  if (!stmt) {
    return sql_failure("prepare list", stmt.error());
  }
  std::vector<entry> out;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return sql_failure("read queue entries", stepped.error());
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto row = read_entry(*stmt);
    if (!row) {
      return std::unexpected(std::move(row.error()));
    }
    out.push_back(std::move(*row));
  }
  return out;
}

} // namespace planar::engine::hostqueue
