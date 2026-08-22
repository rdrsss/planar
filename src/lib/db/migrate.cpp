/// @file migrate.cpp
/// @brief Implementation of `planar.db.migrate` (see migrate.cppm).

module;

module planar.db.migrate;

import std;
import planar.db;
import planar.db.migrations;

namespace planar::db {

auto current_version(connection& conn) -> std::expected<std::uint32_t, db_error> {
  auto stmt = conn.prepare("select coalesce(max(version), 0) from schema_migrations");
  if (!stmt) {
    // `schema_migrations` does not exist yet on a fresh database — treat
    // that as "nothing applied yet" rather than surfacing the prepare
    // failure, matching zig/src/db/migrate.zig's applyAll/
    // assertSchemaCompatible fallback.
    return std::uint32_t{0};
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(step.error());
  }
  if (*step != step_result::row) {
    return std::uint32_t{0};
  }
  return static_cast<std::uint32_t>(stmt->column_int64(0));
}

auto apply_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error> {
  auto current = current_version(conn);
  if (!current) {
    return std::unexpected(current.error());
  }

  for (const auto& m : chain) {
    if (m.version_ <= *current) {
      continue;
    }

    auto txn = conn.begin_transaction();
    if (!txn) {
      return std::unexpected(txn.error());
    }

    // `execute` runs sqlite3_exec over the whole (possibly multi-
    // statement) script; on failure `txn` goes out of scope without a
    // commit and rolls back automatically — the failing migration never
    // partially lands.
    auto exec = conn.execute(m.up_sql_);
    if (!exec) {
      return std::unexpected(exec.error());
    }

    auto commit = txn->commit();
    if (!commit) {
      return std::unexpected(commit.error());
    }
  }

  return {};
}

auto apply_all(connection& conn) -> std::expected<void, db_error> {
  return apply_all(conn, migrations());
}

auto rollback_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error> {
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    auto exec = conn.execute(it->down_sql_);
    if (!exec) {
      return std::unexpected(exec.error());
    }
  }
  return {};
}

auto rollback_all(connection& conn) -> std::expected<void, db_error> {
  return rollback_all(conn, migrations());
}

} // namespace planar::db
