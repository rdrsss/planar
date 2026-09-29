/// @file migrations_agent.cppm
/// @brief `planar.db.migrations_agent` — the compile-time embedded
/// migration chain of the agent database (plan 1080, decision 1181). The
/// second stream `cmake/generate_migrations.cmake` embeds: this interface
/// unit is hand-authored and stable, and the implementation unit that
/// defines `agent::migrations()` is generated at configure time into the
/// build tree from `migrations-agent/*.up.sql` / `*.down.sql`, sorted
/// explicitly by the `NNNNN` filename prefix exactly as the main stream is.
///
/// Records reuse `migration_record` from `planar.db.migrations`. That
/// module is imported here without `export`, so importing the agent chain
/// does NOT put the main stream's `planar::db::migrations()` in scope: a
/// consumer that wants the main chain says so by importing
/// `planar.db.migrations` itself, and one that only applies the agent
/// chain (such as `planar.db.agentdb`) cannot reach for the wrong accessor
/// by accident. The non-exported using-declaration below gives the
/// generated implementation unit the record type under this namespace;
/// importers see the type through the exported signature and name it, when
/// they need to, as `planar::db::migration_record` after importing the main
/// stream. The two chains never mix: this module holds no file from
/// `migrations/`, and `planar.db.migrations` holds no file from
/// `migrations-agent/`.

module;

export module planar.db.migrations_agent;

import std;
import planar.db.migrations;

namespace planar::db::agent {

/// @brief The record type the generated chain is built from, visible to
/// this module's implementation unit without re-exporting the main stream.
using planar::db::migration_record;

/// @brief The full embedded agent migration chain, in ascending version
/// order — generated at configure time from `migrations-agent/*.up.sql`,
/// explicitly sorted by the `NNNNN` filename prefix.
/// @return An ordered, immutable view over every embedded agent migration.
export auto migrations() -> std::span<migration_record const>;

} // namespace planar::db::agent
