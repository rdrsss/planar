/// @file migrations.cppm
/// @brief `planar.db.migrations` — the compile-time embedded migration
/// chain (tech-spec § "Embedded migrations and templates", D5). This
/// interface unit is hand-authored and stable; the implementation unit
/// that actually defines `migrations()` is generated at configure time by
/// `cmake/generate_migrations.cmake` into the build tree (never the source
/// tree) and `#embed`s every `migrations/*.up.sql` / `*.down.sql` pair,
/// enumerated in explicit lexicographic order by the `NNNNN` filename
/// prefix — never `file(GLOB)`'s underlying directory order.
///
/// Same public shape as the Zig codegen it replaces
/// (`zig/tools/gen_migrations.zig`): an ordered, immutable view over every
/// migration's version, name, and up/down SQL text (D2, behavior-
/// preserving).

module;

export module planar.db.migrations;

import std;

namespace planar::db {

/// @brief One embedded migration: a version, a short name, and its forward
/// (`up_sql_`) / reverse (`down_sql_`) SQL scripts, embedded at compile
/// time via `#embed`.
export struct migration_record {
  std::uint32_t    version_ = 0; ///< The `NNNNN` filename prefix, as a plain integer (e.g. `1` for `00001_...`).
  std::string_view name_;        ///< The filename's snake_case description (e.g. `"foundation"`).
  std::string_view up_sql_;      ///< The full `.up.sql` script text, embedded at compile time.
  std::string_view down_sql_;    ///< The full `.down.sql` script text, embedded at compile time.
};

/// @brief The full embedded migration chain, in ascending version order —
/// generated at configure time from `migrations/*.up.sql`, explicitly
/// sorted by the `NNNNN` filename prefix.
/// @return An ordered, immutable view over every embedded migration.
export auto migrations() -> std::span<migration_record const>;

} // namespace planar::db
