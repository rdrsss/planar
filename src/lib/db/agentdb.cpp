/// @file agentdb.cpp
/// @brief Implementation of `planar.db.agentdb`.
module;

#include <cstdlib>

module planar.db.agentdb;

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations_agent;

namespace planar::db::agent {

auto process_env() -> env_lookup {
  return [](std::string_view name) -> std::optional<std::string> {
    std::string const owned(name);
    char const*       value = std::getenv(owned.c_str());
    return value == nullptr ? std::nullopt : std::optional{std::string{value}};
  };
}

auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup {
  return [table = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto const it = table.find(name);
    return it == table.end() ? std::nullopt : std::optional{it->second};
  };
}

auto resolve_agent_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, open_error> {
  if (auto const explicit_path = env(k_agent_db_env); explicit_path.has_value()) {
    if (explicit_path->empty()) {
      return std::unexpected(open_error{
          .kind    = open_error_kind::unresolved_path,
          .message = std::format("cannot locate the agent database: {} is set but empty", k_agent_db_env),
      });
    }
    return std::filesystem::path{*explicit_path};
  }
  if (auto const home = env(k_home_env); home.has_value() && !home->empty()) {
    return std::filesystem::path{*home} / ".planar" / "agent.db";
  }
  return std::unexpected(open_error{
      .kind    = open_error_kind::unresolved_path,
      .message = std::format("cannot locate the agent database: neither {} nor {} is set", k_agent_db_env, k_home_env),
  });
}

auto open_agent_db_at(const std::filesystem::path& path) -> std::expected<connection, open_error> {
  // The parent directory is created here, as the main database's consumer
  // open does (src/cmd/planar-agent/database.cpp), so a fresh `~/.planar`
  // is not a reason to fail. A failure is reported rather than ignored:
  // SQLite would fail to create the file a moment later with a less
  // specific message.
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return std::unexpected(open_error{
          .kind    = open_error_kind::unwritable_location,
          .path    = path,
          .message = std::format("cannot create the agent database directory {} for {}: {}", path.parent_path().string(),
                                 path.string(), ec.message()),
      });
    }
  }

  // `connection::open` sets `busy_timeout` and `journal_mode = WAL` once
  // per read-write connection, so the agent store gets exactly the main
  // database's settings without restating them here.
  auto opened = connection::open(path.string());
  if (!opened) {
    return std::unexpected(open_error{
        .kind        = open_error_kind::open_failed,
        .path        = path,
        .message     = std::format("failed to open agent database {}: {}", path.string(), opened.error().message_),
        .sqlite_code = opened.error().code_,
    });
  }

  // The agent stream, against its own version table. `apply_contiguous`
  // reads `agent_schema_migrations`, applies only what is pending, and is
  // a no-op on an up-to-date store.
  if (auto applied = apply_contiguous(*opened, migrations(), k_agent_version_table); !applied) {
    return std::unexpected(open_error{
        .kind        = open_error_kind::migrate_failed,
        .path        = path,
        .message     = std::format("failed to migrate agent database {}: {}", path.string(), applied.error().message_),
        .sqlite_code = applied.error().code_,
    });
  }

  return std::move(*opened);
}

auto open_agent_db(const env_lookup& env) -> std::expected<connection, open_error> {
  auto path = resolve_agent_db_path(env);
  if (!path) {
    return std::unexpected(std::move(path.error()));
  }
  return open_agent_db_at(*path);
}

} // namespace planar::db::agent
