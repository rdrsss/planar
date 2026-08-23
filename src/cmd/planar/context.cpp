/// @file context.cpp
/// @brief Implementation of `planar.cmd.planar.context`.

module;

#include <cstdlib>

module planar.cmd.planar.context;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar.exit;

namespace planar::cmd {

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
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
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

  // Single-level parent creation, matching the Zig runtime's own
  // "Creates the parent directory if it doesn't exist (single level —
  // covers `~/.planar/`)". create_directories rather than create_directory
  // so a scratch path several levels deep (every test fixture) works too;
  // an already-present directory is not an error either way.
  if (_db_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(_db_path.parent_path(), ec);
  }

  auto opened = db::connection::open(_db_path.string());
  if (!opened) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("failed to open database {}: {}", _db_path.string(), opened.error().message_)));
  }
  _db.emplace(std::move(*opened));

  if (auto applied = db::apply_all(*_db); !applied) {
    _db.reset();
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("migration failed: {}", applied.error().message_)));
  }

  auto const stored = db::current_version(*_db);
  if (!stored) {
    _db.reset();
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("reading schema version failed: {}", stored.error().message_)));
  }
  auto const    chain        = db::migrations();
  std::uint32_t embedded_max = 0;
  for (auto const& record : chain) {
    embedded_max = std::max(embedded_max, record.version_);
  }
  if (*stored > embedded_max) {
    _db.reset();
    return std::unexpected(
        error_from_body(domain_error_kind::schema_version_ahead,
                        std::format("database schema version {} is newer than this binary supports ({}); upgrade planar", *stored,
                                    embedded_max)));
  }

  return &*_db;
}

} // namespace planar::cmd
