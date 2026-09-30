/// @file agentdb.cpp
/// @brief Implementation of `planar.db.agentdb`.
module;

#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

module planar.db.agentdb;

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations_agent;

namespace planar::db::agent {

namespace {

/// @brief Creates every missing component of `dir`, each with mode 0700
/// (before the umask, which can only remove bits). Components that already
/// exist are left exactly as they are. Decision 1210.
auto create_private_directories(const std::filesystem::path& dir) -> std::error_code {
  std::vector<std::filesystem::path> missing;
  std::error_code                    ec;
  for (auto current = dir; !current.empty() && !std::filesystem::exists(current, ec); current = current.parent_path()) {
    missing.push_back(current);
    if (current == current.parent_path()) {
      break;
    }
  }
  for (auto const& component : std::views::reverse(missing)) {
    if (::mkdir(component.c_str(), 0700) != 0 && errno != EEXIST) {
      return std::error_code{errno, std::generic_category()};
    }
  }
  // An existing component that is not a directory (a regular file in the
  // way) is as much a failure to create `dir` as a refused mkdir.
  if (!std::filesystem::is_directory(dir, ec)) {
    return std::make_error_code(std::errc::not_a_directory);
  }
  return {};
}

/// @brief Tightens `dir` to 0700 when the current user owns it and it grants
/// anything to group or others. Best effort: a directory that cannot be
/// tightened is left as it is, since the store's own 0600 mode still holds.
void tighten_owned_directory(const std::filesystem::path& dir) {
  struct ::stat info{};
  if (::stat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
    return;
  }
  if ((info.st_mode & 0077) != 0) {
    static_cast<void>(::chmod(dir.c_str(), 0700));
  }
}

/// @brief Creates the empty store file with mode 0600 when it does not exist,
/// so SQLite opens it instead of creating it with the umask's default mode.
/// SQLite gives the `-wal` and `-shm` files the main file's mode. An existing
/// file is never touched, and a failure is left for SQLite to report with its
/// own, more specific message.
void create_private_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0) {
    ::close(fd);
  }
}

} // namespace

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

auto agent_schema_version() -> std::uint32_t {
  return embedded_max(migrations());
}

auto check_compat(connection& conn, const std::filesystem::path& path) -> std::expected<void, open_error> {
  auto const read_failed = [&](db_error const& error) {
    return std::unexpected(open_error{
        .kind        = open_error_kind::open_failed,
        .path        = path,
        .message     = std::format("failed to read the schema version of agent database {}: {}", path.string(), error.message_),
        .sqlite_code = error.code_,
    });
  };

  // A fresh store has no version table yet; `current_version` treats the
  // same state as version 0. Asking sqlite_master first keeps a genuine
  // read failure below distinguishable from "not created yet".
  auto exists =
      conn.prepare(std::format("select count(*) from sqlite_master where type = 'table' and name = '{}'", k_agent_version_table));
  if (!exists) {
    return read_failed(exists.error());
  }
  if (auto step = exists->step(); !step) {
    return read_failed(step.error());
  } else if (*step != step_result::row || exists->column_int64(0) == 0) {
    return {};
  }

  auto highest = conn.prepare(std::format("select version, compat from {} order by version desc limit 1", k_agent_version_table));
  if (!highest) {
    return read_failed(highest.error());
  }
  auto step = highest->step();
  if (!step) {
    return read_failed(step.error());
  }
  if (*step != step_result::row) {
    return {}; // An empty table: nothing applied, nothing to refuse.
  }
  auto const store_version = static_cast<std::uint32_t>(highest->column_int64(0));
  auto const store_compat  = static_cast<std::uint32_t>(highest->column_int64(1));
  auto const binary        = agent_schema_version();
  if (store_compat <= binary) {
    return {};
  }
  return std::unexpected(open_error{
      .kind           = open_error_kind::incompatible_store,
      .path           = path,
      .message        = std::format("agent database {} needs a newer binary: its schema version {} requires at least "
                                    "agent schema version {} (compat), and this binary's agent schema version is {}",
                                    path.string(), store_version, store_compat, binary),
      .store_compat   = store_compat,
      .binary_version = binary,
  });
}

auto open_agent_db_at(const std::filesystem::path& path) -> std::expected<connection, open_error> {
  // The parent directory is created here, as the main database's consumer
  // open does (src/cmd/planar-agent/database.cpp), so a fresh `~/.planar`
  // is not a reason to fail. A failure is reported rather than ignored:
  // SQLite would fail to create the file a moment later with a less
  // specific message.
  if (path.has_parent_path()) {
    auto const ec = create_private_directories(path.parent_path());
    if (ec) {
      return std::unexpected(open_error{
          .kind    = open_error_kind::unwritable_location,
          .path    = path,
          .message = std::format("cannot create the agent database directory {} for {}: {}", path.parent_path().string(),
                                 path.string(), ec.message()),
      });
    }
  }
  // Decision 1210: the store holds task claim tokens, so it is created
  // owner-only. An existing file keeps the mode it has; the installer
  // tightens those.
  create_private_file(path);

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

  // Before any write: a store whose highest row's `compat` is above this
  // binary's agent schema version is refused as it is, so a newer binary's
  // meaning-changing migration is never half-read or built upon. Reads
  // only, so a refused file keeps its bytes.
  if (auto compatible = check_compat(*opened, path); !compatible) {
    return std::unexpected(std::move(compatible.error()));
  }

  // The agent stream, against its own version table. `apply_contiguous`
  // reads `agent_schema_migrations`, applies only what is pending, and is
  // a no-op on an up-to-date store (or on one ahead of the chain).
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

auto open_agent_db_read_only_at(const std::filesystem::path& path) -> std::expected<connection, open_error> {
  // `open_read_only` fails on a missing file, but with SQLite's terse
  // message; naming the path and the cause here keeps a caller's refusal
  // self-explanatory. The file is never created.
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return std::unexpected(open_error{
        .kind    = open_error_kind::open_failed,
        .path    = path,
        .message = std::format("agent database {} does not exist", path.string()),
    });
  }
  auto opened = connection::open_read_only(path.string());
  if (!opened) {
    return std::unexpected(open_error{
        .kind        = open_error_kind::open_failed,
        .path        = path,
        .message     = std::format("failed to open agent database {} read-only: {}", path.string(), opened.error().message_),
        .sqlite_code = opened.error().code_,
    });
  }
  if (auto compatible = check_compat(*opened, path); !compatible) {
    return std::unexpected(std::move(compatible.error()));
  }
  return std::move(*opened);
}

auto open_agent_db_read_only(const env_lookup& env) -> std::expected<connection, open_error> {
  auto path = resolve_agent_db_path(env);
  if (!path) {
    return std::unexpected(std::move(path.error()));
  }
  return open_agent_db_read_only_at(*path);
}

auto open_agent_db(const env_lookup& env) -> std::expected<connection, open_error> {
  auto path = resolve_agent_db_path(env);
  if (!path) {
    return std::unexpected(std::move(path.error()));
  }
  // Decision 1210: at the default location the directory is Planar's own
  // (`$HOME/.planar`), so it is ensured 0700. A directory the user chose
  // through PLANAR_AGENT_DB is theirs and may hold other things: it is never
  // chmodded, only created 0700 when it did not exist.
  if (!env(k_agent_db_env).has_value() && path->has_parent_path()) {
    tighten_owned_directory(path->parent_path());
  }
  return open_agent_db_at(*path);
}

} // namespace planar::db::agent
