/// @file coordination.cpp
/// @brief Implementation of `planar.coordination`; see coordination.cppm.

module planar.coordination;

import std;
import planar.db;

namespace planar::coordination {

namespace {

/// @brief Run a query whose first column is an id and collect every row.
/// @param conn An open database connection.
/// @param sql A statement with `parameters` integer placeholders, all bound
/// to `task_id`, and an `order by` over its first column.
/// @param task_id The value bound to every placeholder.
/// @param text The optional text value bound after the integers.
/// @return The ids in query order, or the failure.
[[nodiscard]] auto collect_ids(db::connection& conn, std::string_view sql, int parameters, std::int64_t task_id,
                               std::optional<std::string_view> text) -> std::expected<std::vector<std::int64_t>, error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(error::query_failed);
  }
  for (int i = 1; i <= parameters; ++i) {
    if (!stmt->bind_int64(i, task_id)) {
      return std::unexpected(error::query_failed);
    }
  }
  if (text.has_value() && !stmt->bind_text(parameters + 1, *text)) {
    return std::unexpected(error::query_failed);
  }
  std::vector<std::int64_t> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(stmt->column_int64(0));
  }
  return out;
}

/// @brief The linked entities of `kind` whose status is `status`, either direction.
[[nodiscard]] auto linked_with_status(db::connection& conn, std::string_view kind, std::string_view status, std::int64_t task_id)
    -> std::expected<std::vector<std::int64_t>, error> {
  // `kind` is a module-internal literal (`question` or `decision`), never
  // operator input, so interpolating the table name is safe.
  const auto sql = std::format("select distinct e.id from entity_links el join {0}s e on e.id = "
                               "(case when el.from_kind = 'task' then el.to_id else el.from_id end) "
                               "where ((el.from_kind = 'task' and el.from_id = ? and el.to_kind = '{0}') "
                               "or (el.to_kind = 'task' and el.to_id = ? and el.from_kind = '{0}')) "
                               "and e.status = ? order by e.id",
                               kind);
  return collect_ids(conn, sql, 2, task_id, status);
}

} // namespace

auto open_questions_linked(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<std::int64_t>, error> {
  return linked_with_status(conn, "question", open_question_status, task_id);
}

auto proposed_decisions_linked(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<std::int64_t>, error> {
  return linked_with_status(conn, "decision", proposed_decision_status, task_id);
}

auto unfinished_dependencies(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<std::int64_t>, error> {
  return collect_ids(conn,
                     "select distinct t.id from entity_links el join tasks t on t.id = el.to_id "
                     "where el.from_kind = 'task' and el.from_id = ? and el.to_kind = 'task' "
                     "and el.relationship = 'depends-on' and t.id != el.from_id "
                     "and t.status not in ('done', 'cancelled') order by t.id",
                     1, task_id, std::nullopt);
}

} // namespace planar::coordination
