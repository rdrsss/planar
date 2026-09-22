/// @file render_cli.cpp
/// @brief Implementation of `planar.engine.workbench.render_cli` (plan 996,
/// task 6037). Every literal below is oracle-captured from a real run
/// against a scratch database and workbench root.

module planar.engine.workbench.render_cli;

import std;
import planar.json_text;
import planar.engine.workbench.gc;
import planar.engine.workbench.lint;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::render_cli {

namespace {

/// @brief Append `"key":"<escaped>"`.
///
/// `planar.json_text` is this tree's ONE escaper (ten hand-rolled copies
/// were swept in commit 5728133, three of them subtly wrong). Everything
/// operator-facing goes through it.
auto field_string(std::string& out, std::string_view key, std::string_view value) -> void {
  out += '"';
  out += key;
  out += "\":";
  json_text::append_json_string(out, value);
}

auto field_number(std::string& out, std::string_view key, std::int64_t value) -> void {
  out += std::format("\"{}\":{}", key, value);
}

auto field_bool(std::string& out, std::string_view key, bool value) -> void {
  out += std::format("\"{}\":{}", key, value ? "true" : "false");
}

/// @brief Left-align `text` in a field of `width`, never truncating.
///
/// Reproduces zig's `{s:<40}`: a value longer than the width is written
/// whole and the column simply runs wide.
auto pad_right(std::string_view text, std::size_t width) -> std::string {
  std::string out{text};
  while (out.size() < width) {
    out += ' ';
  }
  return out;
}

auto conflict_line(const sync::entry& item) -> std::string {
  return std::format("  CONFLICT [{}]: {} ({} {})\n", item.conflict_id, item.file_path, item.entity_kind, item.entity_id);
}

auto append_conflict_summary(std::string& out, std::size_t conflicts) -> void {
  if (conflicts > 0) {
    out += std::format("  {} conflict(s) - run 'workbench resolve <event-id> --prefer fs|db'\n", conflicts);
  }
}

auto render_sync_result_verbose(std::int64_t plan_id, std::string_view plan_slug, sync::mode run_mode, std::string_view verb,
                                const sync::result& value) -> std::string {
  std::string out       = std::format("workbench {}: plan {} ({})\n", verb, plan_id, plan_slug);
  bool const  writes_fs = run_mode == sync::mode::push || run_mode == sync::mode::sync;
  bool const  writes_db = run_mode == sync::mode::pull || run_mode == sync::mode::sync;
  for (auto const& item : value.entries) {
    switch (item.value) {
    case sync::classification::no_op:
      break;
    case sync::classification::fs_to_db:
      out += writes_db ? std::format("  applied FS->DB: {}\n", item.file_path)
                       : std::format("  pending FS->DB: {}  [run pull to apply]\n", item.file_path);
      break;
    case sync::classification::db_to_fs:
      out += writes_fs ? std::format("  applied DB->FS: {}\n", item.file_path)
                       : std::format("  pending DB->FS: {}  [run push to apply]\n", item.file_path);
      break;
    case sync::classification::conflict:
      out += conflict_line(item);
      break;
    case sync::classification::new_on_fs:
      out += writes_db ? std::format("  new entity: {} -> {} {}\n", item.file_path, item.entity_kind, item.entity_id)
                       : std::format("  new on FS:  {}\n", item.file_path);
      break;
    case sync::classification::deleted_on_fs:
      out += writes_db ? std::format("  deleted: {} -> {} {} cancelled\n", item.file_path, item.entity_kind, item.entity_id)
                       : std::format("  missing: {}\n", item.file_path);
      break;
    case sync::classification::malformed:
      out += item.parse_error.empty() ? std::format("  MALFORMED: {}\n", item.file_path)
                                      : std::format("  MALFORMED: {} ({})\n", item.file_path, item.parse_error);
      break;
    }
  }
  append_conflict_summary(out, value.conflicts);
  return out;
}

} // namespace

auto render_sync_result_text(std::int64_t plan_id, std::string_view plan_slug, sync::mode run_mode, std::string_view verb,
                             const sync::result& value, bool verbose) -> std::string {
  if (verbose) {
    return render_sync_result_verbose(plan_id, plan_slug, run_mode, verb, value);
  }

  std::string out;
  if (run_mode == sync::mode::push) {
    out += std::format("workbench {}: plan {} ({}) - {} applied, {} pending, {} filtered (mode={}), {} conflict(s)", verb,
                       plan_id, plan_slug, value.applied, value.pending, value.filtered, value.filter_mode, value.conflicts);
  } else {
    out += std::format("workbench {}: plan {} ({}) - {} applied, {} pending, {} conflict(s)", verb, plan_id, plan_slug,
                       value.applied, value.pending, value.conflicts);
  }
  if (value.malformed > 0) {
    out += std::format(", {} MALFORMED", value.malformed);
  }
  out += '\n';

  if (run_mode == sync::mode::push) {
    if (value.pre_existing_terminal > 0 && value.cleaned == 0) {
      out += std::format("  {} pre-existing terminal file(s) on disk \xe2\x80\x94 run 'planar workbench gc {}' to remove, "
                         "or re-push with --apply-cleanup\n",
                         value.pre_existing_terminal, plan_id);
    } else if (value.cleaned > 0) {
      out += std::format("  {} pre-existing terminal file(s) cleaned\n", value.cleaned);
    }
  }

  for (auto const& item : value.entries) {
    if (item.value == sync::classification::conflict) {
      out += conflict_line(item);
    }
  }
  append_conflict_summary(out, value.conflicts);
  return out;
}

auto render_sync_result_json(const sync::result& value) -> std::string {
  std::string out = "{";
  field_number(out, "applied", static_cast<std::int64_t>(value.applied));
  out += ',';
  field_number(out, "pending", static_cast<std::int64_t>(value.pending));
  out += ',';
  field_number(out, "conflicts", static_cast<std::int64_t>(value.conflicts));
  out += ',';
  field_number(out, "malformed", static_cast<std::int64_t>(value.malformed));
  out += ",\"malformed_files\":[";
  bool first = true;
  for (auto const& item : value.malformed_files) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    field_string(out, "path", item.path);
    out += ',';
    field_string(out, "parse_error", item.parse_error);
    out += '}';
  }
  out += "],";
  field_number(out, "filtered", static_cast<std::int64_t>(value.filtered));
  out += ',';
  field_number(out, "pre_existing_terminal", static_cast<std::int64_t>(value.pre_existing_terminal));
  out += ',';
  field_number(out, "cleaned", static_cast<std::int64_t>(value.cleaned));
  out += ',';
  field_string(out, "filter_mode", value.filter_mode);
  out += ",\"entries\":[";
  first = true;
  for (auto const& item : value.entries) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    field_string(out, "class", sync::classification_name(item.value));
    out += ',';
    field_string(out, "file_path", item.file_path);
    out += ',';
    field_string(out, "entity_kind", item.entity_kind);
    out += ',';
    field_number(out, "entity_id", item.entity_id);
    out += ',';
    field_number(out, "conflict_id", item.conflict_id);
    out += ',';
    field_string(out, "parse_error", item.parse_error);
    out += '}';
  }
  out += "]}\n";
  return out;
}

auto render_status_totals_json(const status_totals& totals) -> std::string {
  std::string out = "{";
  field_number(out, "applied", static_cast<std::int64_t>(totals.applied));
  out += ',';
  field_number(out, "pending", static_cast<std::int64_t>(totals.pending));
  out += ',';
  field_number(out, "conflicts", static_cast<std::int64_t>(totals.conflicts));
  out += ',';
  field_number(out, "malformed", static_cast<std::int64_t>(totals.malformed));
  out += ",\"malformed_files\":[";
  bool first = true;
  for (auto const& item : totals.malformed_files) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    field_string(out, "path", item.path);
    out += ',';
    field_string(out, "parse_error", item.parse_error);
    out += '}';
  }
  out += "],";
  field_number(out, "filtered", static_cast<std::int64_t>(totals.filtered));
  out += ',';
  field_number(out, "pre_existing_terminal", static_cast<std::int64_t>(totals.pre_existing_terminal));
  out += ',';
  field_number(out, "cleaned", static_cast<std::int64_t>(totals.cleaned));
  out += "}\n";
  return out;
}

auto render_list_json(std::span<const sync::active_feature> items) -> std::string {
  std::string out   = "[";
  bool        first = true;
  for (auto const& item : items) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    field_number(out, "plan", item.plan_id);
    out += ',';
    field_string(out, "slug", item.slug);
    out += ',';
    field_string(out, "status", item.status);
    out += ',';
    // `assoc`, not `assoc_slug` — the JSON key differs from the struct
    // field name here. Oracle-captured.
    field_string(out, "assoc", item.assoc_slug);
    out += ',';
    field_string(out, "plan_key", item.plan_key);
    out += ',';
    field_bool(out, "has_fs_tree", item.has_fs_tree);
    out += '}';
  }
  out += "]\n";
  return out;
}

auto render_list_text(std::span<const sync::active_feature> items) -> std::string {
  if (items.empty()) {
    return "no features found\n";
  }
  std::string out;
  for (auto const& item : items) {
    out += std::format("{}  {}  {}  {}\n", pad_right(std::format("{}-{}", item.plan_key, item.slug), 40),
                       pad_right(item.status, 10), pad_right(item.has_fs_tree ? "tree" : "no-tree", 10), item.assoc_slug);
  }
  return out;
}

auto render_gc_json(const gc::summary& value, bool dry_run, terminal::mode filter_mode) -> std::string {
  return std::format("{{\"removed\":{},\"kept\":{},\"drifted_skipped\":{},\"errors\":{},\"dry_run\":{},"
                     "\"filter_mode\":\"{}\"}}\n",
                     value.removed, value.kept, value.drifted_skipped, value.errors, dry_run ? "true" : "false",
                     terminal::mode_to_string(filter_mode));
}

auto render_gc_text(const gc::summary& value, bool dry_run, terminal::mode filter_mode) -> std::string {
  if (dry_run) {
    return std::format("workbench gc (--dry-run): would remove {}, keep {}, drifted-skipped {}, errors {} (mode={})\n",
                       value.removed, value.kept, value.drifted_skipped, value.errors, terminal::mode_to_string(filter_mode));
  }
  return std::format("workbench gc: removed {}, kept {}, drifted-skipped {}, errors {} (mode={})\n", value.removed, value.kept,
                     value.drifted_skipped, value.errors, terminal::mode_to_string(filter_mode));
}

auto render_gc_drift_refusal(const gc::summary& value) -> std::string {
  std::string out = std::format("workbench gc: refused to remove {} file(s) with FS-content drift from DB; re-run with --yes to "
                                "discard, or 'workbench pull' first\n",
                                value.drifted_skipped);
  for (auto const& path : value.drifted_paths) {
    out += std::format("  drift: {}\n", path);
  }
  return out;
}

auto render_lint_json(std::span<const lint::issue> issues) -> std::string {
  std::string out;
  for (auto const& item : issues) {
    out += '{';
    field_string(out, "path", item.path);
    out += ',';
    field_number(out, "line", static_cast<std::int64_t>(item.line));
    out += ',';
    field_string(out, "severity", lint::severity_name(item.level));
    out += ',';
    field_string(out, "code", item.code);
    out += ',';
    field_string(out, "message", item.message);
    out += ',';
    field_string(out, "hint", item.hint);
    out += "}\n";
  }
  return out;
}

auto render_lint_text(const lint::result& value) -> std::string {
  std::string out;
  for (auto const& item : value.issues) {
    out += std::format("{}:{}:\n  {}[{}]: {}\n", item.path, item.line, lint::severity_name(item.level), item.code, item.message);
    if (!item.hint.empty()) {
      out += std::format("  hint: {}\n", item.hint);
    }
  }
  out += std::format("{} files scanned, {} errors, {} warnings.\n", value.files_scanned, value.errors, value.warnings);
  return out;
}

auto render_archive(std::int64_t plan_id, std::string_view feature_dir, bool json) -> std::string {
  if (!json) {
    return std::format("archived: {} (plan {})\n", feature_dir, plan_id);
  }
  std::string out = "{";
  field_bool(out, "archived", true);
  out += ',';
  field_number(out, "plan", plan_id);
  out += ',';
  field_string(out, "feature_dir", feature_dir);
  out += "}\n";
  return out;
}

auto render_restore(std::int64_t plan_id, std::string_view feature_dir, bool json) -> std::string {
  if (!json) {
    return std::format("restored: {} (plan {})\n", feature_dir, plan_id);
  }
  std::string out = "{";
  field_bool(out, "restored", true);
  out += ',';
  field_number(out, "plan", plan_id);
  out += ',';
  field_string(out, "feature_dir", feature_dir);
  out += "}\n";
  return out;
}

auto render_resolve(std::int64_t event_id, sync::conflict_resolution prefer, bool json) -> std::string {
  std::string_view const label = prefer == sync::conflict_resolution::fs ? "fs" : "db";
  if (!json) {
    return std::format("resolved conflict event {} (preferred {})\n", event_id, label);
  }
  // Hand-formatted rather than field-by-field: the Zig handler writes this
  // one with a raw `print` too, and both `event_id` and `prefer` are
  // constrained (an integer and one of two literals).
  return std::format("{{\"resolved\":true,\"event_id\":{},\"prefer\":\"{}\"}}\n", event_id, label);
}

auto render_no_active_features() -> std::string {
  return "no active features found\n";
}

auto error_body_malformed(std::size_t count, std::optional<std::int64_t> plan_id) -> std::string {
  if (plan_id) {
    return std::format("{} malformed workbench file(s); run 'planar workbench lint {}' for details", count, *plan_id);
  }
  return std::format("{} malformed workbench file(s); run 'planar workbench lint --all' for details", count);
}

auto error_body_conflicts(std::size_t count) -> std::string {
  return std::format("{} conflict(s) require 'workbench resolve <event-id> --prefer fs|db'", count);
}

auto error_body_lint_issues(std::size_t errors, std::size_t warnings) -> std::string {
  return std::format("workbench lint found {} error(s) and {} warning(s)", errors, warnings);
}

} // namespace planar::engine::workbench::render_cli
