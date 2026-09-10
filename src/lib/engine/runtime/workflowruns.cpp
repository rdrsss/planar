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

auto read(db::statement const& q) -> run {
  return {.id             = q.column_int64(0),
          .plan_id        = q.column_int64(1),
          .pid            = q.column_int64(4),
          .workflow_name  = q.column_text(2),
          .run_identifier = q.column_text(3),
          .repo_root      = q.column_text(5),
          .status         = q.column_text(6)};
}

auto lookup(db::connection& c, std::string_view identifier) -> std::expected<run, error> {
  auto q =
      c.prepare("select id,plan_id,workflow_name,run_identifier,pid,repo_root,status from workflow_runs where run_identifier=?");
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

  auto insert = c.prepare("insert into workflow_runs(plan_id,workflow_name,run_identifier,pid,repo_root) values(?,?,?,?,?)");
  if (!insert || !insert->bind_int64(1, input.plan_id) || !insert->bind_text(2, input.workflow_name) ||
      !insert->bind_text(3, input.run_identifier) || !insert->bind_int64(4, input.pid) || !insert->bind_text(5, input.repo_root))
    return db_failure();
  if (!insert->step())
    return db_failure();
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
} // namespace planar::engine::runtime::workflowruns
