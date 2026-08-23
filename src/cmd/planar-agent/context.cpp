/// @file context.cpp
/// @brief Implementation of `planar.cmd.planar_agent.context`.

module;

#include <cstdlib>

module planar.cmd.planar_agent.context;

import std;
import planar.cli;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

namespace {

/// @brief The highest migration version this binary embeds — the Zig
/// original's `db.migrate.embedded_max`.
/// @return The embedded maximum schema version.
auto embedded_max() -> std::uint32_t {
  std::uint32_t highest = 0;
  for (auto const& record : db::migrations()) {
    highest = std::max(highest, record.version_);
  }
  return highest;
}

/// @brief The live schema version, with a missing/unreadable
/// `schema_migrations` table mapping to 0.
///
/// Zero is not an error here and that is deliberate, straight from
/// zig/src/runtime/runtime.zig: "A missing `schema_migrations` table
/// (fresh DB never touched by `planar init`) maps to version 0 — which
/// trips SchemaVersionBehind below as long as the binary's embedded
/// migrations include anything at all." Reporting it as a read failure
/// instead would give the operator a SQLite diagnostic where the real
/// answer is "run `planar init`".
/// @param conn The open connection.
/// @return The live version, or 0.
auto live_version(db::connection& conn) -> std::uint32_t {
  auto const stored = db::current_version(conn);
  if (!stored) {
    return 0;
  }
  return *stored;
}

} // namespace

auto process_env() -> env_lookup {
  return [](std::string_view name) -> std::optional<std::string> {
    std::string const owned(name);
    const char*       value = std::getenv(owned.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string{value};
  };
}

auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup {
  return [table = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto const it = table.find(name);
    if (it == table.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error> {
  if (auto const explicit_path = env("PLANAR_DB"); explicit_path.has_value()) {
    return std::filesystem::path{*explicit_path};
  }
  auto const home = env("HOME");
  if (!home.has_value()) {
    return std::unexpected(error_from_body(cli::domain_error_kind::generic_failure,
                                           "neither PLANAR_DB nor HOME is set; cannot locate the Planar database"));
  }
  return std::filesystem::path{*home} / ".planar" / "planar.db";
}

auto operator_cwd(const env_lookup& env) -> std::filesystem::path {
  if (auto const pwd = env("PWD"); pwd.has_value() && !pwd->empty()) {
    return std::filesystem::path{*pwd};
  }
  std::error_code ec;
  auto            here = std::filesystem::current_path(ec);
  if (ec) {
    return std::filesystem::path{};
  }
  return here;
}

auto context::ensure_db() -> std::expected<db::connection*, domain_error> {
  if (_db.has_value()) {
    return &*_db;
  }

  // The consumer path DOES create the parent directory, matching
  // ensureDbConsumerImpl (only the STRICT read-only path skips it).
  if (_db_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(_db_path.parent_path(), ec);
  }

  auto opened = db::connection::open(_db_path.string());
  if (!opened) {
    return std::unexpected(
        error_from_body(cli::domain_error_kind::generic_failure,
                        std::format("failed to open database {}: {}", _db_path.string(), opened.error().message_)));
  }
  _db.emplace(std::move(*opened));

  // Both PRAGMAs are best-effort and degrade to a warning, exactly as the
  // Zig original does — a failure here slows things down, it does not make
  // the answer wrong, so it must not abort the verb.
  if (auto const wal = _db->execute("PRAGMA journal_mode = WAL"); !wal) {
    err() << std::format("warning: failed to set journal_mode=WAL ({}); follow / dashboard latency may degrade\n",
                         wal.error().message_);
  }
  if (auto const busy = _db->execute("PRAGMA busy_timeout = 5000"); !busy) {
    err() << std::format("warning: failed to set busy_timeout ({}); concurrent writers may see SQLITE_BUSY\n",
                         busy.error().message_);
  }

  // NO apply_all. `planar init` owns migration; this binary consumes the
  // schema. See this module's header.
  auto const stored  = live_version(*_db);
  auto const maximum = embedded_max();

  if (stored < maximum) {
    err() << std::format("error: schema version {} in {} is older than this binary's minimum of {}; "
                         "run `planar init` to apply migrations\n",
                         stored, _db_path.string(), maximum);
    _db.reset();
    return std::unexpected(error_from_body(cli::domain_error_kind::schema_version_behind, "SchemaVersionBehind"));
  }
  if (stored > maximum) {
    err() << std::format("error: schema version {} in {} is newer than this binary's embedded max ({}); "
                         "the DB was migrated by a newer build — rebuild/reinstall planar from a checkout "
                         "whose migrations include version {} (e.g. once that migration lands on master), "
                         "then retry\n",
                         stored, _db_path.string(), maximum, stored);
    _db.reset();
    return std::unexpected(error_from_body(cli::domain_error_kind::schema_version_ahead, "SchemaVersionAhead"));
  }

  return &*_db;
}

} // namespace planar::cmd::agent
