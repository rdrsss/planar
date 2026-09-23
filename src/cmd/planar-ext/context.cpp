/// @file context.cpp
/// @brief Implementation of `planar.cmd.planar_ext.context`.

module;

#include <cstdlib>

module planar.cmd.planar_ext.context;

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {

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

auto write_allowlist() -> std::vector<std::string> {
  // Decision 995's exact write surface. ORDER is not meaningful; kept
  // alphabetical for readability.
  return {"external_links", "external_systems", "sync_events"};
}

auto context::ensure_db() -> std::expected<db::connection*, domain_error> {
  if (_db.has_value()) {
    return &*_db;
  }

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

  // The write-capability boundary. See this module's header — this is the
  // enforcement point decision 995 requires, not a call sites-audited
  // convention.
  auto const allowlist = write_allowlist();
  _db->restrict_writes_to(allowlist);

  // `journal_mode = WAL` and `busy_timeout = 5000` are set once, centrally,
  // by `db::connection::open` itself (task 6842, decision D7) — this binary
  // always opens read/write, so no duplicate pragma is needed here.

  // NO apply_all. `planar init` owns migration; this binary consumes the
  // schema.
  // The schema-version handshake. The comparison lives in
  // `planar.db.migrate` (see `schema_compatibility`'s decision record —
  // task 6058); this binary's POLICY (refuse both directions) and its
  // wording stay here, pinned by context.t.cpp.
  //
  // A read failure degrades to version 0 rather than surfacing a SQLite
  // diagnostic, which is what the local `live_version` helper this
  // replaced did: "A missing `schema_migrations` table (fresh DB never
  // touched by `planar init`) maps to version 0 — which trips
  // SchemaVersionBehind below as long as the binary's embedded
  // migrations include anything at all." The answer an operator needs
  // there is "run `planar init`", not a driver error string.
  auto const compat  = db::assert_schema_compatible(*_db);
  auto const stored  = compat ? compat->live_ : std::uint32_t{0};
  auto const maximum = db::embedded_max();

  if (stored < maximum) {
    err() << std::format("error: schema version {} in {} is older than this binary's minimum of {}; "
                         "run `planar init` to apply migrations\n",
                         stored, _db_path.string(), maximum);
    _db.reset();
    return std::unexpected(error_from_body(domain_error_kind::schema_version_behind, "SchemaVersionBehind"));
  }
  if (stored > maximum) {
    err() << std::format("error: schema version {} in {} is newer than this binary's embedded max ({}); "
                         "the DB was migrated by a newer build — rebuild/reinstall planar from a checkout "
                         "whose migrations include version {} (e.g. once that migration lands on master), "
                         "then retry\n",
                         stored, _db_path.string(), maximum, stored);
    _db.reset();
    return std::unexpected(error_from_body(domain_error_kind::schema_version_ahead, "SchemaVersionAhead"));
  }

  // Version matches, but the applied set has a hole in it: warn and
  // proceed. See `schema_compatibility`'s decision record, answer (3) —
  // a corrupt applied-set is precisely when an operator needs to be able
  // to LOOK at state, and the missing structure announces itself at the
  // first query that touches it.
  if (compat && compat->verdict_ == db::schema_compatibility::gap) {
    err() << std::format("warning: schema_migrations in {} reports version {} but its applied set has a hole; the "
                         "database is missing structure this binary expects. Inspect `select version from "
                         "schema_migrations order by version` before trusting any result\n",
                         _db_path.string(), compat->live_);
  }

  return &*_db;
}

} // namespace planar::cmd::ext
