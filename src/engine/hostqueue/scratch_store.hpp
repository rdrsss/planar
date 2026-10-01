// scratch_store.hpp: the one place the hostqueue engine tests choose which
// migration chain a scratch store is built from (plan 1089, task
// qp-separability; tech spec 656 § Separability).
//
// The queue tables live in `planar.db` (migration 00040), so every engine
// test store is a scratch `planar.db` migrated from the embedded MAIN chain.
// The retired agent chain is not imported anywhere under this directory.
//
// A header, included rather than linked, exactly as `src/cmd/parity_harness.hpp`
// is: the including test file imports `std`, `planar.db` and
// `planar.db.migrate` first, so every name below is already visible.
#pragma once

/// @brief Opens `path` read-write and brings it to the head of the embedded
/// main chain, creating the file when it does not exist.
/// @param path The scratch database's location; its directory must exist.
/// @return The connection, or the SQLite failure from the open or a migration.
inline auto open_main_store_at(const std::filesystem::path& path) -> std::expected<planar::db::connection, planar::db::db_error> {
  auto opened = planar::db::connection::open(path.string());
  if (!opened) {
    return std::unexpected(opened.error());
  }
  if (auto applied = planar::db::apply_all(*opened); !applied) {
    return std::unexpected(applied.error());
  }
  return opened;
}

/// @brief Opens an existing scratch `planar.db` read-only, as `queue status`
/// and the viewers do. Applies nothing.
/// @param path The database's location.
/// @return The read-only connection, or the SQLite failure.
inline auto open_main_store_read_only_at(const std::filesystem::path& path)
    -> std::expected<planar::db::connection, planar::db::db_error> {
  return planar::db::connection::open_read_only(path.string());
}
