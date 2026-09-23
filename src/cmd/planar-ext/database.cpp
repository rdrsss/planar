/// @file database.cpp
/// @brief Database access policy for this command binary.
module planar.cmd.planar_ext.database;
import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {
auto write_allowlist() -> std::vector<std::string> {
  return {"external_links", "external_systems", "sync_events"};
}

auto database_policy::open(const std::filesystem::path& path, std::ostream& error_stream)
    -> std::expected<db::connection, domain_error> {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }

  auto opened = db::connection::open(path.string());
  if (!opened) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("failed to open database {}: {}", path.string(), opened.error().message_)));
  }
  std::optional<db::connection> connection{std::move(*opened)};

  // The write-capability boundary. See this module's header — this is the
  // enforcement point decision 995 requires, not a call sites-audited
  // convention.
  auto const allowlist = write_allowlist();
  connection->restrict_writes_to(allowlist);

  // The connection layer sets WAL and busy_timeout once at open (D7).

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
  auto const compat  = db::assert_schema_compatible(*connection);
  auto const stored  = compat ? compat->live_ : std::uint32_t{0};
  auto const maximum = db::embedded_max();

  if (stored < maximum) {
    error_stream << std::format("error: schema version {} in {} is older than this binary's minimum of {}; "
                                "run `planar init` to apply migrations\n",
                                stored, path.string(), maximum);
    return std::unexpected(error_from_body(domain_error_kind::schema_version_behind, "SchemaVersionBehind"));
  }
  if (stored > maximum) {
    error_stream << std::format("error: schema version {} in {} is newer than this binary's embedded max ({}); "
                                "the DB was migrated by a newer build — rebuild/reinstall planar from a checkout "
                                "whose migrations include version {} (e.g. once that migration lands on master), "
                                "then retry\n",
                                stored, path.string(), maximum, stored);
    return std::unexpected(error_from_body(domain_error_kind::schema_version_ahead, "SchemaVersionAhead"));
  }

  // Version matches, but the applied set has a hole in it: warn and
  // proceed. See `schema_compatibility`'s decision record, answer (3) —
  // a corrupt applied-set is precisely when an operator needs to be able
  // to LOOK at state, and the missing structure announces itself at the
  // first query that touches it.
  if (compat && compat->verdict_ == db::schema_compatibility::gap) {
    error_stream << std::format("warning: schema_migrations in {} reports version {} but its applied set has a hole; the "
                                "database is missing structure this binary expects. Inspect `select version from "
                                "schema_migrations order by version` before trusting any result\n",
                                path.string(), compat->live_);
  }

  return std::move(*connection);
}

} // namespace planar::cmd::ext
