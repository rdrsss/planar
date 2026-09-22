/// @file workflowruns.cpp
/// @brief Implementation of the `workflow_runs` lifecycle store.
module planar.engine.runtime.workflowruns;

import std;
import planar.db;

namespace planar::engine::runtime::workflowruns {
namespace {
auto db_failure() -> std::unexpected<error> {
  return std::unexpected(error::query_failed);
}

/// @brief Read a nullable integer column.
/// @param q The statement, positioned on a row.
/// @param index The 0-indexed column.
/// @return The value, or unset when NULL.
auto opt_int(const db::statement& q, int index) -> std::optional<std::int64_t> {
  if (q.is_null(index)) {
    return std::nullopt;
  }
  return q.column_int64(index);
}

/// @brief Read a nullable text column.
/// @param q The statement, positioned on a row.
/// @param index The 0-indexed column.
/// @return The value, or unset when NULL.
auto opt_text(const db::statement& q, int index) -> std::optional<std::string> {
  if (q.is_null(index)) {
    return std::nullopt;
  }
  return q.column_text(index);
}

auto read(db::statement const& q) -> run {
  return {.id             = q.column_int64(0),
          .plan_id        = q.column_int64(1),
          .pid            = opt_int(q, 4),
          .expires_at     = opt_text(q, 7),
          .workflow_name  = q.column_text(2),
          .run_identifier = q.column_text(3),
          .repo_root      = q.column_text(5),
          .status         = q.column_text(6)};
}

auto lookup(db::connection& c, std::string_view identifier) -> std::expected<run, error> {
  auto q = c.prepare("select id,plan_id,workflow_name,run_identifier,pid,repo_root,status,expires_at from workflow_runs "
                     "where run_identifier=?");
  if (!q || !q->bind_text(1, identifier))
    return db_failure();
  auto stepped = q->step();
  if (!stepped)
    return db_failure();
  if (*stepped == db::step_result::done)
    return std::unexpected(error::run_not_found);
  return read(*q);
}
} // namespace

auto terminal_status(std::string_view value) -> bool {
  return value == "completed" || value == "failed" || value == "interrupted";
}

auto start(db::connection& c, const start_input& input) -> std::expected<run, error> {
  auto plan = c.prepare("select 1 from plans where id=?");
  if (!plan || !plan->bind_int64(1, input.plan_id))
    return db_failure();
  auto present = plan->step();
  if (!present)
    return db_failure();
  if (*present == db::step_result::done)
    return std::unexpected(error::plan_not_found);

  // Exactly one of pid / ttl_secs is expected (the caller — the CLI
  // handler — validates that before this is reached; the migration-00039
  // CHECK is the database's own backstop either way).
  if (input.pid.has_value()) {
    auto insert = c.prepare("insert into workflow_runs(plan_id,workflow_name,run_identifier,pid,repo_root) values(?,?,?,?,?)");
    if (!insert || !insert->bind_int64(1, input.plan_id) || !insert->bind_text(2, input.workflow_name) ||
        !insert->bind_text(3, input.run_identifier) || !insert->bind_int64(4, *input.pid) ||
        !insert->bind_text(5, input.repo_root))
      return db_failure();
    if (!insert->step())
      return db_failure();
  } else {
    auto insert = c.prepare("insert into workflow_runs(plan_id,workflow_name,run_identifier,repo_root,expires_at) "
                            "values(?,?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ','now','+'||?||' seconds'))");
    if (!insert || !insert->bind_int64(1, input.plan_id) || !insert->bind_text(2, input.workflow_name) ||
        !insert->bind_text(3, input.run_identifier) || !insert->bind_text(4, input.repo_root) ||
        !insert->bind_int64(5, input.ttl_secs.value_or(0)))
      return db_failure();
    if (!insert->step())
      return db_failure();
  }
  return lookup(c, input.run_identifier);
}

auto find(db::connection& c, std::string_view identifier) -> std::expected<run, error> {
  return lookup(c, identifier);
}

auto end(db::connection& c, std::string_view identifier, std::string_view status) -> std::expected<run, error> {
  auto tx = c.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return db_failure();
  auto found = lookup(c, identifier);
  if (!found)
    return std::unexpected(found.error());
  if (found->status != "running")
    return std::unexpected(error::not_running);
  auto update = c.prepare(
      "update workflow_runs set status=?, ended_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=? and status='running'");
  if (!update || !update->bind_text(1, status) || !update->bind_int64(2, found->id) || !update->step())
    return db_failure();
  if (!tx->commit())
    return db_failure();
  found->status = std::string{status};
  return found;
}

auto heartbeat(db::connection& c, std::string_view identifier, std::int64_t ttl_secs) -> std::expected<run, error> {
  auto tx = c.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return db_failure();
  auto found = lookup(c, identifier);
  if (!found)
    return std::unexpected(found.error());
  if (found->status != "running")
    return std::unexpected(error::not_running);
  if (found->pid.has_value())
    return std::unexpected(error::pid_bound);
  auto update = c.prepare("update workflow_runs set expires_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','+'||?||' seconds') "
                          "where id=? and status='running' and pid is null");
  if (!update || !update->bind_int64(1, ttl_secs) || !update->bind_int64(2, found->id) || !update->step())
    return db_failure();
  if (!tx->commit())
    return db_failure();
  return lookup(c, identifier);
}
} // namespace planar::engine::runtime::workflowruns
