/// @file manifest.cpp
/// @brief Implementation of `planar.engine.workbench.manifest` (plan 996,
/// task 6037), including the bucket-local SHA-256. See manifest.cppm for why
/// the digest is implemented here rather than vendored.

module planar.engine.workbench.manifest;

import std;
import planar.db;
import planar.engine.workbench.fsutil;
import planar.sha256;

namespace planar::engine::workbench::manifest {

namespace {

constexpr std::string_view k_select_columns = "select id, anchor_plan_id, entity_kind, entity_id, file_path, content_hash, "
                                              "coalesce(fs_mtime, ''), coalesce(db_updated_at, ''), coalesce(last_synced_at, '') "
                                              "from workbench_sync_state where anchor_plan_id = ? order by file_path";

} // namespace

auto hash_content(std::string_view content) -> std::string {
  // Layer-1 `planar.sha256` (task 6759). This file used to carry its own
  // FIPS 180-4 -- one of five in the tree, counted by round constants -- and
  // hex-encode the raw digest itself. `content_hash` is a STORED value that
  // drives every freshness verdict, so the copies were compared as ORDERED
  // constant sequences (a sorted-set comparison cannot see a misordering,
  // which is what task 6106 actually fixed) and confirmed identical before
  // any was removed.
  return sha256::hex(content);
}

auto load(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<std::vector<sync_state>, manifest_error> {
  auto stmt = conn.prepare(k_select_columns);
  if (!stmt) {
    return std::unexpected(manifest_error::query_failed);
  }
  if (!stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(manifest_error::query_failed);
  }
  std::vector<sync_state> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(manifest_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(sync_state{
        .id             = stmt->column_int64(0),
        .anchor_plan_id = stmt->column_int64(1),
        .entity_kind    = stmt->column_text(2),
        .entity_id      = stmt->column_int64(3),
        .file_path      = stmt->column_text(4),
        .content_hash   = stmt->column_text(5),
        .fs_mtime       = stmt->column_text(6),
        .db_updated_at  = stmt->column_text(7),
        .last_synced_at = stmt->column_text(8),
    });
  }
  return out;
}

auto upsert(db::connection& conn, const sync_state& row) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("insert into workbench_sync_state "
                           "(anchor_plan_id, entity_kind, entity_id, file_path, content_hash, fs_mtime, "
                           "db_updated_at, last_synced_at) "
                           "values (?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now')) "
                           "on conflict(file_path) do update set "
                           "  anchor_plan_id = excluded.anchor_plan_id, "
                           "  entity_kind = excluded.entity_kind, "
                           "  entity_id = excluded.entity_id, "
                           "  content_hash = excluded.content_hash, "
                           "  fs_mtime = excluded.fs_mtime, "
                           "  db_updated_at = excluded.db_updated_at, "
                           "  last_synced_at = excluded.last_synced_at");
  if (!stmt) {
    return std::unexpected(manifest_error::query_failed);
  }
  bool const bound = stmt->bind_int64(1, row.anchor_plan_id).has_value() && stmt->bind_text(2, row.entity_kind).has_value() &&
                     stmt->bind_int64(3, row.entity_id).has_value() && stmt->bind_text(4, row.file_path).has_value() &&
                     stmt->bind_text(5, row.content_hash).has_value() && stmt->bind_text(6, row.fs_mtime).has_value() &&
                     stmt->bind_text(7, row.db_updated_at).has_value();
  if (!bound) {
    return std::unexpected(manifest_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_by_file_path(db::connection& conn, std::string_view file_path) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where file_path = ?");
  if (!stmt || !stmt->bind_text(1, file_path) || !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_by_entity(db::connection& conn, std::int64_t anchor_plan_id, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where anchor_plan_id = ? and entity_kind = ? and entity_id = ?");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_text(2, entity_kind) || !stmt->bind_int64(3, entity_id) ||
      !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_for_plan(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where anchor_plan_id = ?");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto feature_rel_path(std::string_view file_path) -> std::string_view {
  auto trimmed = file_path;
  while (!trimmed.empty() && trimmed.front() == '/') {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() && trimmed.back() == '/') {
    trimmed.remove_suffix(1);
  }
  auto const first = trimmed.find('/');
  if (first == std::string_view::npos) {
    return trimmed;
  }
  auto const second = trimmed.find('/', first + 1);
  if (second == std::string_view::npos) {
    return trimmed;
  }
  return trimmed.substr(second + 1);
}

auto render_sync_file(std::span<const sync_state> rows) -> std::string {
  std::string out;
  for (auto const& row : rows) {
    std::format_to(std::back_inserter(out), "{}\t{}:{}\t{}\t{}\n", feature_rel_path(row.file_path), row.entity_kind,
                   row.entity_id, row.content_hash, row.last_synced_at);
  }
  return out;
}

auto write_sync_file(const std::filesystem::path& feature_dir, std::span<const sync_state> rows) -> bool {
  return fsutil::write_file_atomic(feature_dir / ".sync", render_sync_file(rows));
}

} // namespace planar::engine::workbench::manifest
