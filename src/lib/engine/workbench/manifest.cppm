/// @file manifest.cppm
/// @brief `planar.engine.workbench.manifest` — the `workbench_sync_state`
/// table and its on-disk `.sync` mirror (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/manifest.zig.
///
/// ## What the manifest IS
///
/// One row per projected file, keyed by `file_path` (which carries a UNIQUE
/// constraint — the upsert below relies on it). Each row remembers the
/// content hash and the DB `updated_at` AS OF the last successful sync.
/// That pair is the entire basis of drift detection: comparing the file's
/// current hash against `content_hash` says whether the FS changed, and
/// comparing the entity's current `updated_at` against `db_updated_at` says
/// whether the DB changed. Both changed, differently, is a conflict.
///
/// ## `.sync` is a MIRROR, not the source of truth
///
/// After any writing run the feature directory gets a `.sync` file listing
/// the same rows, tab-separated, for humans and for external tooling:
///
///     tasks/cross/1-first-task.md\ttask:1\t<sha256>\t2026-08-23T15:00:41.075Z\n
///
/// Oracle-captured, including the trailing newline on every line and the
/// FEATURE-relative (not root-relative) path in column one. Nothing reads
/// it back — losing it costs nothing; the table is authoritative.
///
/// ## The hash
///
/// Lowercase hex SHA-256 of the file's exact bytes. SHA-256 is implemented
/// in this module (manifest.cpp) rather than pulled in: the tree vendors
/// Catch2, SQLite, Glaze and spdlog and none of them exposes one, and
/// adding a crypto dependency for a 64-character content fingerprint would
/// be out of proportion. It is verified against the oracle's own `.sync`
/// digests and against `shasum -a 256` in manifest.t.cpp, so a
/// transcription slip fails a test rather than silently making every file
/// look drifted.
module;

export module planar.engine.workbench.manifest;

import std;
import planar.db;

namespace planar::engine::workbench::manifest {

/// @brief One `workbench_sync_state` row.
export struct sync_state {
  std::int64_t id             = 0; ///< Row id; 0 for a value being written.
  std::int64_t anchor_plan_id = 0; ///< The owning anchor plan.
  std::string  entity_kind;        ///< Canonical entity kind.
  std::int64_t entity_id = 0;      ///< The backing row id.
  std::string  file_path;          ///< ROOT-relative stored path; the row's unique key.
  std::string  content_hash;       ///< Lowercase hex SHA-256 as of the last sync.
  std::string  fs_mtime;           ///< Always empty in practice (see below).
  std::string  db_updated_at;      ///< The entity's `updated_at` as of the last sync.
  std::string  last_synced_at;     ///< Set by SQLite on write.
};

/// @brief Failure surface for the manifest's DB operations.
export enum class manifest_error : std::uint8_t {
  query_failed, ///< SQLite refused a prepare, bind or step.
};

/// @brief Lowercase hex SHA-256 of `content`.
/// @param content The bytes to digest.
/// @return A 64-character lowercase hex string.
export auto hash_content(std::string_view content) -> std::string;

/// @brief Every manifest row for an anchor plan, ordered by `file_path`.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @return The rows, or the failure.
export auto load(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<std::vector<sync_state>, manifest_error>;

/// @brief Insert or replace the row for `row.file_path`.
///
/// `last_synced_at` is set by SQLite (`strftime('%Y-%m-%dT%H:%M:%fZ','now')`)
/// rather than by the caller, so the value on the row passed in is ignored.
/// @param conn The database connection.
/// @param row The row to write.
/// @return Success, or the failure.
export auto upsert(db::connection& conn, const sync_state& row) -> std::expected<void, manifest_error>;

/// @brief Delete the row for one stored path.
/// @param conn The database connection.
/// @param file_path The ROOT-relative stored path.
/// @return Success, or the failure.
export auto delete_by_file_path(db::connection& conn, std::string_view file_path) -> std::expected<void, manifest_error>;

/// @brief Delete the row for one entity within one anchor plan.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param entity_kind The entity kind.
/// @param entity_id The entity id.
/// @return Success, or the failure.
export auto delete_by_entity(db::connection& conn, std::int64_t anchor_plan_id, std::string_view entity_kind,
                             std::int64_t entity_id) -> std::expected<void, manifest_error>;

/// @brief Delete every row for an anchor plan (what `archive` does).
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @return Success, or the failure.
export auto delete_for_plan(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, manifest_error>;

/// @brief The exact bytes of a `.sync` file for `rows`.
///
/// A COMPLETE payload: every line, terminator included. An empty row list
/// is ZERO bytes, not a bare newline.
/// @param rows The manifest rows, in the order they should appear.
/// @return The file contents.
export auto render_sync_file(std::span<const sync_state> rows) -> std::string;

/// @brief Write `<feature_dir>/.sync` atomically.
/// @param feature_dir The feature directory.
/// @param rows The manifest rows.
/// @return `true` on success.
export auto write_sync_file(const std::filesystem::path& feature_dir, std::span<const sync_state> rows) -> bool;

/// @brief The FEATURE-relative part of a ROOT-relative stored path — that
/// is, everything after the second `/`.
///
/// Reproduced verbatim including its edge case: a path with FEWER than two
/// separators is returned WHOLE. That happens for a global-scope feature
/// (no association level), whose `.sync` therefore lists
/// `p1-slug/README.md` rather than `README.md`. Arguably wrong, definitely
/// what ships, and `.sync` is a mirror nothing reads back — so it is
/// reproduced rather than corrected (D2).
/// @param file_path The ROOT-relative stored path.
/// @return The feature-relative remainder.
export auto feature_rel_path(std::string_view file_path) -> std::string_view;

} // namespace planar::engine::workbench::manifest
