/// @file database.cpp
/// @brief Database access policy for this command binary.
module planar.cmd.planar.database;
import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar.exit;

namespace planar::cmd {
auto database_policy::open(const std::filesystem::path& path, std::ostream& error_stream)
    -> std::expected<db::connection, domain_error> {
  // Single-level parent creation, matching the Zig runtime's own
  // "Creates the parent directory if it doesn't exist (single level —
  // covers `~/.planar/`)". create_directories rather than create_directory
  // so a scratch path several levels deep (every test fixture) works too;
  // an already-present directory is not an error either way.
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

  if (auto applied = db::apply_all(*connection); !applied) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("migration failed: {}", applied.error().message_)));
  }

  // The schema-version handshake. The comparison itself lives in
  // `planar.db.migrate` (see `schema_compatibility`'s decision record —
  // task 6058); what stays HERE is this binary's policy and this binary's
  // wording, both of which differ per binary and are pinned by
  // context.t.cpp / exit_codes.t.cpp.
  auto const state = db::assert_schema_compatible(*connection);
  if (!state) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("reading schema version failed: {}", state.error().message_)));
  }
  if (state->verdict_ == db::schema_compatibility::ahead) {
    return std::unexpected(error_from_body(
        domain_error_kind::schema_version_ahead,
        std::format("database schema version {} is newer than this binary supports ({}); the DB was migrated by a "
                    "newer build -- rebuild/reinstall planar from a checkout whose migrations include version {}, "
                    "then retry",
                    state->live_, state->embedded_max_, state->live_)));
  }
  // NO `behind` arm, and that is the operator binary's defining
  // difference: it just migrated. `planar` owns migration; the three
  // consumer binaries refuse `behind` and point back here.
  if (state->verdict_ == db::schema_compatibility::gap) {
    // Warn, do not refuse — see `schema_compatibility`'s decision record,
    // answer (3). Refusing here would take `planar health` away from the
    // operator at the exact moment they need it.
    error_stream << std::format("warning: schema_migrations in {} reports version {} but its applied set has a hole; some "
                                "migration was rolled back or deleted without its successors -- the database is missing "
                                "structure this binary expects. Inspect `select version from schema_migrations order by "
                                "version` before trusting any result\n",
                                path.string(), state->live_);
  }

  return std::move(*connection);
}

} // namespace planar::cmd
