/// @file schema.cpp
/// @brief Implementation of `planar.engine.hostqueue.schema` (plan 1089,
/// task qp-queue-compat). See schema.cppm for the contract.

module planar.engine.hostqueue.schema;

import std;
import planar.db;

namespace planar::engine::hostqueue {

namespace {

/// @brief A `query_failed` error naming the step and SQLite's reason.
auto query_failure(std::string_view what, const db::db_error& err) -> queue_schema_error {
  return queue_schema_error{
      .kind    = queue_schema_failure::query_failed,
      .message = std::format("queue schema check: {}: {} (sqlite {})", what, err.message_, err.code_),
  };
}

/// @brief Whether `table` exists as a table on `conn`.
auto table_exists(db::connection& conn, std::string_view table) -> std::expected<bool, queue_schema_error> {
  auto stmt = conn.prepare("select count(*) from sqlite_master where type = 'table' and name = ?");
  if (!stmt) {
    return std::unexpected(query_failure("look up a queue table", stmt.error()));
  }
  if (auto bound = stmt->bind_text(1, table); !bound) {
    return std::unexpected(query_failure("look up a queue table", bound.error()));
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(query_failure("look up a queue table", stepped.error()));
  }
  return *stepped == db::step_result::row && stmt->column_int64(0) > 0;
}

/// @brief The column names `table` has on `conn`.
auto columns_of(db::connection& conn, std::string_view table) -> std::expected<std::set<std::string>, queue_schema_error> {
  auto stmt = conn.prepare("select name from pragma_table_info(?)");
  if (!stmt) {
    return std::unexpected(query_failure("read the queue table columns", stmt.error()));
  }
  if (auto bound = stmt->bind_text(1, table); !bound) {
    return std::unexpected(query_failure("read the queue table columns", bound.error()));
  }
  std::set<std::string> out;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(query_failure("read the queue table columns", stepped.error()));
    }
    if (*stepped == db::step_result::done) {
      return out;
    }
    out.insert(stmt->column_text(0));
  }
}

/// @brief Appends `table.column` to `missing` for every listed column `table`
/// lacks.
auto collect_missing(db::connection& conn, std::string_view table, std::span<std::string_view const> listed,
                     std::vector<std::string>& missing) -> std::expected<void, queue_schema_error> {
  auto present = columns_of(conn, table);
  if (!present) {
    return std::unexpected(std::move(present.error()));
  }
  for (auto const column : listed) {
    if (!present->contains(std::string(column))) {
      missing.push_back(std::format("{}.{}", table, column));
    }
  }
  return {};
}

} // namespace

auto protocol_value(std::string_view name) -> std::optional<std::string_view> {
  for (auto const entry : k_queue_protocol) {
    auto const eq = entry.find('=');
    if (eq != std::string_view::npos && entry.substr(0, eq) == name) {
      return entry.substr(eq + 1);
    }
  }
  return std::nullopt;
}

auto check_queue_schema(db::connection& conn) -> std::expected<void, queue_schema_error> {
  // 1. The three tables exist.
  for (auto const table :
       {std::string_view{"queue_schema"}, std::string_view{"queue_entries"}, std::string_view{"queue_history"}}) {
    auto exists = table_exists(conn, table);
    if (!exists) {
      return std::unexpected(std::move(exists.error()));
    }
    if (!*exists) {
      return std::unexpected(queue_schema_error{
          .kind    = queue_schema_failure::missing_table,
          .message = std::format("the database has no {} table", table),
      });
    }
  }

  // 2. The marker's highest row admits this binary.
  auto marker = conn.prepare("select version, compat from queue_schema order by version desc limit 1");
  if (!marker) {
    return std::unexpected(query_failure("read queue_schema", marker.error()));
  }
  auto stepped = marker->step();
  if (!stepped) {
    return std::unexpected(query_failure("read queue_schema", stepped.error()));
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(queue_schema_error{
        .kind    = queue_schema_failure::incompatible_marker,
        .message = "queue_schema has no rows, so the queue tables' version is unknown",
    });
  }
  auto const version = static_cast<std::uint32_t>(marker->column_int64(0));
  auto const compat  = static_cast<std::uint32_t>(marker->column_int64(1));
  if (compat > k_queue_schema_version) {
    return std::unexpected(queue_schema_error{
        .kind          = queue_schema_failure::incompatible_marker,
        .message       = std::format("the queue tables are at queue version {} and need a binary at queue version {} or "
                                     "later; this binary is at queue version {}",
                                     version, compat, k_queue_schema_version),
        .store_version = version,
        .store_compat  = compat,
    });
  }

  // 3. The column guard: every column this binary names exists.
  std::vector<std::string> missing;
  if (auto collected = collect_missing(conn, "queue_entries", k_queue_entries_columns, missing); !collected) {
    return std::unexpected(std::move(collected.error()));
  }
  if (auto collected = collect_missing(conn, "queue_history", k_queue_history_columns, missing); !collected) {
    return std::unexpected(std::move(collected.error()));
  }
  if (!missing.empty()) {
    std::string names;
    for (auto const& name : missing) {
      names += names.empty() ? "" : ", ";
      names += name;
    }
    return std::unexpected(queue_schema_error{
        .kind          = queue_schema_failure::missing_column,
        .message       = std::format("the queue tables lack {} {} this binary uses (queue_schema version {}, compat {})",
                                     missing.size() == 1 ? "a column" : "columns", names, version, compat),
        .store_version = version,
        .store_compat  = compat,
    });
  }
  return {};
}

auto queue_schema_refusal_tag(std::uint32_t live, std::uint32_t embedded) -> std::optional<std::string_view> {
  if (live == embedded) {
    return k_tag_queue_schema_foreign;
  }
  if (live > embedded) {
    return k_tag_queue_schema_incompatible;
  }
  return std::nullopt;
}

} // namespace planar::engine::hostqueue
