/// @file database.cpp
/// @brief Database access policy for this command binary.
module planar.cmd.planar_watch.database;
import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {
auto database_policy::open(const std::filesystem::path& path, std::ostream& error_stream)
    -> std::expected<db::connection, domain_error> {
  // NO create_directories, and no fallback to a writable open. A viewer
  // that bootstrapped state would not be a viewer. See this module's
  // header.
  auto opened = db::connection::open_read_only(path.string());
  if (!opened) {
    // `OpenFailed` — the oracle's error TAG, and nothing else.
    //
    // TASK 6120 CHANGED THIS LINE, and the reason is worth recording. It
    // used to render `failed to open database <path>: <sqlite message>`,
    // which is strictly more informative. It was also UNREACHABLE: until
    // the six read verbs landed, no leaf in this binary opened a database
    // at all, so no test and no operator ever saw either string. The
    // moment `planar-watch ps` existed, the differential against
    // `zig/zig-out/bin/planar-watch` reported it on the first run:
    //
    //     $ planar-watch ps          # against a path with no database
    //     stderr: error: OpenFailed
    //     exit:   1
    //
    // Matched (D2) rather than kept. This is the FIRST thing an operator
    // sees when they point the viewer somewhere wrong, and a script
    // greps it. The path detail is a real loss; the operator can still
    // read the path out of `$PLANAR_DB`, and diverging on the very first
    // stderr line of the very first verb would have been the worse trade.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "OpenFailed"));
  }
  std::optional<db::connection> connection{std::move(*opened)};

  // The read-only connection sets busy_timeout once at open (D7).

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

} // namespace planar::cmd::watch
