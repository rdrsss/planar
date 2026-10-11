/// @file database.cpp
/// @brief Database access policy for this command binary.
module planar.cmd.planar_agent.database;
import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {
auto database_policy::open(const std::filesystem::path& path, std::ostream& error_stream)
    -> std::expected<db::connection, domain_error> {
  // The consumer path DOES create the parent directory, matching
  // ensureDbConsumerImpl (only the STRICT read-only path skips it).
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
  return verify(std::move(*opened), path, error_stream, false);
}

auto database_policy::open_existing(const std::filesystem::path& path, std::ostream& error_stream, int busy_timeout_ms)
    -> std::expected<db::connection, domain_error> {
  auto opened = db::connection::open_existing(path.string(), busy_timeout_ms);
  if (!opened) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("failed to open database {}: {}", path.string(), opened.error().message_)));
  }
  return verify(std::move(*opened), path, error_stream, true);
}

auto database_policy::verify(db::connection opened, const std::filesystem::path& path, std::ostream& error_stream, bool strict)
    -> std::expected<db::connection, domain_error> {
  std::optional<db::connection> connection{std::move(opened)};

  // The connection layer sets WAL and busy_timeout once at open (D7).

  // NO apply_all. `planar init` owns migration; this binary consumes the
  // schema. See this module's header.
  // The schema-version handshake. The comparison lives in
  // `planar.db.migrate` (see `schema_compatibility`'s decision record —
  // task 6058); this binary's POLICY (refuse both directions) and its
  // wording stay here, pinned by context.t.cpp.
  //
  // A missing `schema_migrations` table (fresh DB never touched by
  // `planar init`) comes back as version 0 and trips SchemaVersionBehind
  // below, whose answer is "run `planar init`". Any other read failure is
  // this process being unable to read the file — a sandbox, or file or
  // folder permissions — and is reported as that, never as a schema that
  // needs migrating.
  auto const compat = db::assert_schema_compatible(*connection);
  if (!compat && strict && db::is_busy(compat.error())) {
    // A caller with a bounded wait must be told the version could not be read
    // because of a lock, not that the schema is behind.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("cannot read the schema version of {}", path.string())));
  }
  if (!compat) {
    error_stream << std::format("error: cannot read the database at {}: {}. {}\n", path.string(), compat.error().message_,
                                db::access_requirement());
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "DatabaseUnreadable"));
  }
  auto const stored  = compat->live_;
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

} // namespace planar::cmd::agent
