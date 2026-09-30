/// @file agentstore.cpp
/// @brief Agent database access policy for this command binary.
module planar.cmd.planar_watch.agentstore;

import std;
import planar.db;
import planar.db.agentdb;
import planar.engine.hostqueue;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {

namespace {

/// @brief Turns a queue engine read failure into this binary's error.
auto read_error(const engine::hostqueue::queue_error& error) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("agent database read failed: {}", error.message));
}

/// @brief Whether `conn` has a table called `name`. A store a submitter has
/// created but not yet migrated has none, and reads as empty.
auto has_table(db::connection& conn, std::string_view name) -> std::expected<bool, domain_error> {
  auto failed = [](const db::db_error& e) {
    return error_from_body(domain_error_kind::generic_failure, std::format("agent database read failed: {}", e.message_));
  };
  auto stmt = conn.prepare(std::format("select count(*) from sqlite_master where type = 'table' and name = '{}'", name));
  if (!stmt) {
    return std::unexpected(failed(stmt.error()));
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(failed(step.error()));
  }
  return *step == db::step_result::row && stmt->column_int64(0) != 0;
}

/// @brief Refuses a file that has tables but is not an agent store: no
/// `agent_schema_migrations` table. A file with no tables at all is a store
/// created and not yet migrated and passes.
auto require_agent_store(db::connection& conn, const std::filesystem::path& path) -> std::expected<void, domain_error> {
  auto stmt = conn.prepare("select count(*), coalesce(sum(name = 'agent_schema_migrations'), 0) from sqlite_master "
                           "where type = 'table' and name not like 'sqlite_%'");
  if (!stmt) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("agent database read failed: {}", stmt.error().message_)));
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("agent database read failed: {}", step.error().message_)));
  }
  if (*step == db::step_result::row && stmt->column_int64(0) > 0 && stmt->column_int64(1) == 0) {
    return std::unexpected(error_from_body(
        domain_error_kind::generic_failure,
        std::format("{} is a database but not an agent store (it has no agent_schema_migrations table); check PLANAR_AGENT_DB",
                    path.string())));
  }
  return {};
}

} // namespace

agent_store::agent_store(std::filesystem::path path, std::optional<db::connection> connection)
    : _path(std::move(path)), _connection(std::move(connection)) {
}
auto agent_store::path() const -> const std::filesystem::path& {
  return _path;
}
auto agent_store::present() const -> bool {
  return _connection.has_value();
}
auto agent_store::connection() -> db::connection* {
  return _connection ? &*_connection : nullptr;
}
auto agent_store::entries() -> std::expected<std::vector<engine::hostqueue::entry>, domain_error> {
  if (!_connection) {
    return std::vector<engine::hostqueue::entry>{};
  }
  auto const table = has_table(*_connection, "queue_entries");
  if (!table) {
    return std::unexpected(table.error());
  }
  if (!*table) {
    return std::vector<engine::hostqueue::entry>{};
  }
  auto listed = engine::hostqueue::list(*_connection);
  if (!listed) {
    return std::unexpected(read_error(listed.error()));
  }
  return std::move(*listed);
}
auto agent_store::history(std::optional<std::int64_t> ended_since)
    -> std::expected<std::vector<engine::hostqueue::history_row>, domain_error> {
  if (!_connection) {
    return std::vector<engine::hostqueue::history_row>{};
  }
  auto const table = has_table(*_connection, "queue_history");
  if (!table) {
    return std::unexpected(table.error());
  }
  if (!*table) {
    return std::vector<engine::hostqueue::history_row>{};
  }
  auto listed = engine::hostqueue::list_history(*_connection, ended_since);
  if (!listed) {
    return std::unexpected(read_error(listed.error()));
  }
  return std::move(*listed);
}

auto agent_open_error(const db::agent::open_error& error) -> domain_error {
  auto const kind = error.kind == db::agent::open_error_kind::incompatible_store ? domain_error_kind::schema_version_ahead
                                                                                 : domain_error_kind::generic_failure;
  return error_from_body(kind, error.message);
}

auto open_agent_store_at(const std::filesystem::path& path) -> std::expected<agent_store, domain_error> {
  // A missing file is an empty queue: nothing has ever been submitted. Only
  // a definite "not found" counts; a status error (unreadable directory) is
  // a fault and falls through to the open, which reports it.
  auto const is_missing = [&path] {
    std::error_code ec;
    auto const      st = std::filesystem::status(path, ec);
    return st.type() == std::filesystem::file_type::not_found;
  };
  if (is_missing()) {
    return agent_store{path, std::nullopt};
  }
  // Read-only, compat-checked, never creating, never migrating.
  auto opened = db::agent::open_agent_db_read_only_at(path);
  if (!opened) {
    if (opened.error().kind == db::agent::open_error_kind::open_failed && is_missing()) {
      return agent_store{path, std::nullopt}; // Removed between the check and the open.
    }
    return std::unexpected(agent_open_error(opened.error()));
  }
  if (auto agent = require_agent_store(*opened, path); !agent) {
    return std::unexpected(std::move(agent.error()));
  }
  return agent_store{path, std::move(*opened)};
}

auto open_agent_store(const agent_env_lookup& env) -> std::expected<agent_store, domain_error> {
  auto path = db::agent::resolve_agent_db_path(env);
  if (!path) {
    return std::unexpected(agent_open_error(path.error()));
  }
  return open_agent_store_at(*path);
}

} // namespace planar::cmd::watch
