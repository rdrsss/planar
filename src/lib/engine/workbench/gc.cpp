/// @file gc.cpp
/// @brief Implementation of `planar.engine.workbench.gc` (plan 996, task
/// 6037). See gc.cppm for the keep rules and the drift refusal.

module planar.engine.workbench.gc;

import std;
import planar.db;
import planar.engine.workbench.feature;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.manifest;
import planar.engine.workbench.parse;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::gc {

namespace {

auto fetch_status(db::connection& conn, std::string_view kind, std::int64_t id) -> std::string {
  std::string_view sql;
  if (kind == "plan") {
    sql = "select coalesce(status, '') from plans where id = ?";
  } else if (kind == "task") {
    sql = "select coalesce(status, '') from tasks where id = ?";
  } else if (kind == "decision") {
    sql = "select coalesce(status, '') from decisions where id = ?";
  } else if (kind == "question") {
    sql = "select coalesce(status, '') from questions where id = ?";
  } else if (kind == "scenario" || kind == "test_scenario") {
    sql = "select coalesce(status, '') from test_scenarios where id = ?";
  } else if (kind == "artifact") {
    sql = "select coalesce(status, '') from artifacts where id = ?";
  } else {
    return {};
  }
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, id)) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

auto fetch_manifest_hash(db::connection& conn, std::int64_t anchor_plan_id, std::string_view kind, std::int64_t id)
    -> std::string {
  auto stmt = conn.prepare("select coalesce(content_hash, '') from workbench_sync_state "
                           "where anchor_plan_id = ? and entity_kind = ? and entity_id = ? limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_text(2, kind) || !stmt->bind_int64(3, id)) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief Sweep one plan into an accumulating summary.
/// @return `false` when the plan could not be walked at all (the caller
/// counts that as one `errors` under `--all-scopes`).
auto run_for_plan_into(db::connection& conn, std::int64_t anchor_plan_id, std::string_view workbench_root, const options& opts,
                       summary& out) -> bool {
  auto anchor_value = sync::fetch_anchor(conn, anchor_plan_id);
  if (!anchor_value) {
    // Not an anchor plan (or gone). The Zig original swallows this
    // silently rather than counting it, on either entry point.
    return true;
  }
  auto const feature_dir = sync::feature_dir_for(workbench_root, *anchor_value);
  if (!fsutil::path_exists(feature_dir)) {
    return true; // No tree, nothing to collect.
  }

  auto files = fsutil::collect_markdown(feature_dir);
  std::ranges::sort(files);
  for (auto const& path : files) {
    auto const content = fsutil::read_file(path);
    if (!content) {
      ++out.errors;
      continue;
    }
    auto const parsed = parse::parse(*content);
    if (!parsed) {
      ++out.kept; // Unparseable front matter: never delete what we cannot read.
      continue;
    }
    auto const& fm     = parsed->frontmatter;
    auto const  status = fetch_status(conn, fm.entity_kind, fm.entity_id);
    auto const  drop   = terminal::is_filtered_str(fm.entity_kind, status, opts.filter_mode);
    if (!drop.has_value() || !*drop) {
      ++out.kept;
      continue;
    }

    auto const fs_hash = manifest::hash_content(*content);
    auto const db_hash = fetch_manifest_hash(conn, anchor_plan_id, fm.entity_kind, fm.entity_id);
    // No manifest row means nothing to have drifted FROM, so an untracked
    // terminal file is removable without `--yes`.
    bool const drifted = !db_hash.empty() && fs_hash != db_hash;
    if (drifted && !opts.yes) {
      out.drifted_paths.push_back(path);
      ++out.drifted_skipped;
      continue;
    }

    if (opts.dry_run) {
      ++out.removed;
      continue;
    }
    if (!fsutil::remove_file(path)) {
      ++out.errors;
      continue;
    }
    ++out.removed;
    static_cast<void>(manifest::delete_by_entity(conn, anchor_plan_id, fm.entity_kind, fm.entity_id));
  }
  return true;
}

} // namespace

auto run_for_plan(db::connection& conn, std::int64_t anchor_plan_id, std::string_view workbench_root, const options& opts)
    -> std::expected<summary, gc_error> {
  summary out;
  static_cast<void>(run_for_plan_into(conn, anchor_plan_id, workbench_root, opts, out));
  return out;
}

auto run_all_scopes(db::connection& conn, std::string_view workbench_root, const options& opts)
    -> std::expected<summary, gc_error> {
  summary out;
  auto    stmt = conn.prepare("select id from plans where parent_plan_id is null order by id");
  if (!stmt) {
    return std::unexpected(gc_error::query_failed);
  }
  std::vector<std::int64_t> plan_ids;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(gc_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    plan_ids.push_back(stmt->column_int64(0));
  }
  // The id list is drained BEFORE any sweep runs: `run_for_plan_into`
  // prepares its own statements on the same connection, and stepping this
  // one interleaved with those would rely on SQLite's statement isolation
  // for no benefit.
  for (auto const plan_id : plan_ids) {
    if (!run_for_plan_into(conn, plan_id, workbench_root, opts, out)) {
      ++out.errors;
    }
  }
  return out;
}

} // namespace planar::engine::workbench::gc
