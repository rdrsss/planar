/// @file audit_trail.cpp
/// @brief Implementation of `planar.engine.runtime.audit_trail` (plan 996,
/// task 6090). See audit_trail.cppm for scope and omissions.

module planar.engine.runtime.audit_trail;

import std;
import planar.db;

namespace planar::engine::runtime::audit_trail {

namespace {

/// @brief The column list every `audit_log` query in this module selects,
/// in the order `read_entry_row` indexes.
constexpr std::string_view k_select_columns = "select id, verb, entity_kind, entity_id, actor, scope, summary, "
                                              "recorded_at from audit_log";

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

auto read_entry_row(const db::statement& stmt) -> audit_entry {
  return audit_entry{
      .id          = stmt.column_int64(0),
      .verb        = stmt.column_text(1),
      .entity_kind = stmt.column_text(2),
      .entity_id   = stmt.column_int64(3),
      .actor       = opt_text(stmt, 4),
      .scope       = opt_text(stmt, 5),
      .summary     = opt_text(stmt, 6),
      .recorded_at = stmt.column_text(7),
  };
}

/// @brief Drain a prepared, already-bound statement into a row vector.
/// @param stmt The bound statement.
/// @return The rows in the statement's own order, or
/// `audit_error::query_failed` on a step failure.
auto drain(db::statement& stmt) -> std::expected<std::vector<audit_entry>, audit_error> {
  std::vector<audit_entry> rows;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return std::unexpected(audit_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(read_entry_row(stmt));
  }
}

} // namespace

auto for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<audit_entry>, audit_error> {
  auto stmt = conn.prepare(std::format("{} where entity_kind = ? and entity_id = ? order by id", k_select_columns));
  if (!stmt) {
    return std::unexpected(audit_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, entity_kind); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, entity_id); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  return drain(*stmt);
}

auto for_entity_with_links(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<audit_entry>, audit_error> {
  auto stmt = conn.prepare(std::format("{} where (entity_kind = ? and entity_id = ?) "
                                       "or (entity_kind, entity_id) in ("
                                       "select to_kind, to_id from entity_links where from_kind = ? and from_id = ? "
                                       "union "
                                       "select from_kind, from_id from entity_links where to_kind = ? and to_id = ?"
                                       ") order by id",
                                       k_select_columns));
  if (!stmt) {
    return std::unexpected(audit_error::query_failed);
  }
  // The same pair is bound three times: once for the direct match, once
  // for the outgoing-edge branch, once for the incoming-edge branch.
  for (int pair = 0; pair < 3; ++pair) {
    if (auto b = stmt->bind_text((pair * 2) + 1, entity_kind); !b) {
      return std::unexpected(audit_error::query_failed);
    }
    if (auto b = stmt->bind_int64((pair * 2) + 2, entity_id); !b) {
      return std::unexpected(audit_error::query_failed);
    }
  }
  return drain(*stmt);
}

auto for_entity_grep(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::string_view pattern)
    -> std::expected<std::vector<audit_entry>, audit_error> {
  auto stmt =
      conn.prepare(std::format("{} where entity_kind = ? and entity_id = ? and summary like ? order by id", k_select_columns));
  if (!stmt) {
    return std::unexpected(audit_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, entity_kind); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, entity_id); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  // No `escape` clause, matching the oracle: `%` and `_` inside `pattern`
  // stay wildcards. See the interface comment.
  if (auto b = stmt->bind_text(3, std::format("%{}%", pattern)); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  return drain(*stmt);
}

auto session_timeline(db::connection& conn, std::int64_t session_id) -> std::expected<session_timeline_result, audit_error> {
  session_timeline_result result;
  result.session_id = session_id;

  {
    auto stmt = conn.prepare("select vendor, task_id, started_at, ended_at from sessions where id = ?");
    if (!stmt) {
      return std::unexpected(audit_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, session_id); !b) {
      return std::unexpected(audit_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(audit_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(audit_error::not_found);
    }
    result.vendor     = stmt->column_text(0);
    result.task_id    = stmt->is_null(1) ? std::nullopt : std::optional<std::int64_t>{stmt->column_int64(1)};
    result.started_at = stmt->column_text(2);
    result.ended_at   = stmt->is_null(3) ? std::nullopt : std::optional<std::string>{stmt->column_text(3)};
  }

  auto stmt = conn.prepare("select ordinal, prefix, body from session_entries where session_id = ? order by ordinal");
  if (!stmt) {
    return std::unexpected(audit_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, session_id); !b) {
    return std::unexpected(audit_error::query_failed);
  }
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(audit_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return result;
    }
    result.entries.push_back(session_entry{
        .ordinal = stmt->column_int64(0),
        .prefix  = stmt->column_text(1),
        .body    = stmt->column_text(2),
    });
  }
}

} // namespace planar::engine::runtime::audit_trail
