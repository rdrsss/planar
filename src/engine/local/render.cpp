/// @file render.cpp
/// @brief Implementation of `planar.engine.local.render` (plan 996, task 6109).
/// See render.cppm for the three JSON envelope conventions and the empty-input
/// disagreements between leaves.

module planar.engine.local.render;

import std;
import planar.json_text;
import planar.engine.local.manifest;
import planar.engine.local.link;
import planar.engine.local.importer;

namespace planar::engine::local::render {

namespace {

using json_text::append_json_string;

/// @brief `"key":"value"` with a leading comma when `first` is already false.
auto append_kv(std::string& out, bool& first, std::string_view key, std::string_view value) -> void {
  if (!first) {
    out.push_back(',');
  }
  first = false;
  append_json_string(out, key);
  out.push_back(':');
  append_json_string(out, value);
}

/// @brief Left-pad `value` to `width` with spaces; never truncates.
///
/// An over-long value pushes the rest of the row right rather than being cut.
/// That is Zig's `{s:<N}` and it is what the oracle emits for a 30-character
/// skill name.
auto pad(std::string_view value, std::size_t width) -> std::string {
  std::string out{value};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

/// @brief The shared `Record` / `Removed[]` object body.
///
/// One function for both because the oracle emits byte-identical objects in
/// `list`, `link` and `unlink` — the SAME six-or-eight keys in the same order.
/// Keeping one function makes it impossible for the three to drift.
///
/// `mode` is `""` when absent (stringified before serialization, so it is never
/// null). `warning` and `linked_at` are OMITTED when empty, because these call
/// sites pass `emit_null_optional_fields = false` and the handler maps empty to
/// null on the way in.
auto append_target_record(std::string& out, const link::target_record& rec) -> void {
  out.push_back('{');
  bool first = true;
  append_kv(out, first, "vendor", rec.vendor);
  append_kv(out, first, "target_path", rec.target_path);
  append_kv(out, first, "source_path", rec.source_path);
  append_kv(out, first, "mode", rec.mode ? manifest::mode_name(*rec.mode) : std::string_view{});
  append_kv(out, first, "action", rec.action);
  if (!rec.warning.empty()) {
    append_kv(out, first, "warning", rec.warning);
  }
  if (!rec.linked_at.empty()) {
    append_kv(out, first, "linked_at", rec.linked_at);
  }
  out.push_back('}');
}

} // namespace

auto list_json(std::span<const link::list_record> rows, std::optional<std::string_view> vendor_filter) -> std::string {
  std::string out;
  for (const auto& row : rows) {
    if (vendor_filter && !link::vendor_matches(row.record.vendor, *vendor_filter)) {
      continue;
    }
    out.push_back('{');
    bool first = true;
    append_kv(out, first, "Name", row.name);
    append_kv(out, first, "Kind", manifest::kind_name(row.kind));
    out.append(",\"Record\":");
    append_target_record(out, row.record);
    out.append("}\n");
  }
  return out;
}

auto list_text(std::span<const link::list_record> rows, std::optional<std::string_view> vendor_filter) -> std::string {
  if (rows.empty()) {
    return "no sandbox installs recorded\n";
  }
  // The header is emitted BEFORE the filter is applied, which is why a filter
  // that matches nothing still prints it. Deliberate: see list_text()'s doc.
  std::string out =
      std::format("{}  {}  {}  {}  {}\n", pad("name", 12), pad("kind", 7), pad("vendor", 7), pad("status", 7), "target");
  bool any = false;
  for (const auto& row : rows) {
    if (vendor_filter && !link::vendor_matches(row.record.vendor, *vendor_filter)) {
      continue;
    }
    any = true;
    out.append(std::format("{}  {}  {}  {}  {}\n", pad(row.name, 12), pad(manifest::kind_name(row.kind), 7),
                           pad(row.record.vendor, 7), pad(row.record.action, 7), row.record.target_path));
  }
  if (!any) {
    out.append("no rows matched filter\n");
  }
  return out;
}

auto link_json(const manifest::sandbox_file& file, const link::link_result& result) -> std::string {
  std::string out   = "{\"Source\":{";
  bool        first = true;
  append_kv(out, first, "SourcePath", file.source_path);
  append_kv(out, first, "Name", file.name);
  append_kv(out, first, "Kind", manifest::kind_name(file.kind));

  out.append(",\"Frontmatter\":{");
  bool fm_first = true;
  append_kv(out, fm_first, "Description", file.frontmatter.description);
  append_kv(out, fm_first, "ArgumentHint", file.frontmatter.argument_hint);
  append_kv(out, fm_first, "Tier", file.frontmatter.tier);
  append_kv(out, fm_first, "Model", file.frontmatter.model);
  // The AUTHORED vendor list, not the resolved one: an author who named none
  // gets `[]` here even though all three were installed into.
  out.append(",\"Vendors\":[");
  for (std::size_t i = 0; i < file.frontmatter.vendors.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_json_string(out, file.frontmatter.vendors[i]);
  }
  out.append("],\"Kind\":");
  append_json_string(out, file.frontmatter.kind);
  out.push_back('}');

  out.append(",\"Body\":");
  append_json_string(out, file.body);
  out.append("},\"Records\":[");
  for (std::size_t i = 0; i < result.records.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_target_record(out, result.records[i]);
  }
  out.append("]}\n");
  return out;
}

auto link_source_text(const link::link_result& result) -> std::string {
  std::string out = std::format("{} ({})\n", result.name, manifest::kind_name(result.kind));
  for (const auto& rec : result.records) {
    if (rec.mode) {
      out.append(std::format("  {}  {} [{}]  ->  {}\n", pad(rec.vendor, 7), rec.action, manifest::mode_name(*rec.mode),
                             rec.target_path));
    } else {
      // No empty `[]`: the bracket group and the space before it are both gone.
      out.append(std::format("  {}  {}  ->  {}\n", pad(rec.vendor, 7), rec.action, rec.target_path));
    }
    if (!rec.warning.empty()) {
      out.append(std::format("           warning: {}\n", rec.warning));
    }
  }
  return out;
}

auto link_summary_text(std::span<const link::link_result> results, bool dry_run) -> std::string {
  std::size_t linked    = 0;
  std::size_t unchanged = 0;
  std::size_t skipped   = 0;
  std::size_t dry       = 0;
  for (const auto& result : results) {
    for (const auto& rec : result.records) {
      if (rec.action == "created" || rec.action == "updated") {
        ++linked;
      } else if (rec.action == "unchanged") {
        ++unchanged;
      } else if (rec.action == "skipped") {
        ++skipped;
      } else if (rec.action == "dry-run") {
        ++dry;
      }
    }
  }
  if (dry_run) {
    return std::format("\ndry-run: {} would-be installs across {} source(s)\n", dry, results.size());
  }
  return std::format("\ndone: {} linked, {} unchanged, {} skipped across {} source(s)\n", linked, unchanged, skipped,
                     results.size());
}

auto walk_error_text(const manifest::walk_error& value) -> std::string {
  return std::format("warning: {}: {}\n", value.path, value.message);
}

auto lint_text(const manifest::lint_issue& issue, manifest::kind file_kind, std::string_view name) -> std::string {
  const std::string_view severity = issue.severity == manifest::lint_severity::warning ? "warning" : "error";
  return std::format("lint [{}] {}/{}.{}: {}\n", severity, manifest::kind_name(file_kind), name, issue.field, issue.message);
}

auto unlink_json(const link::unlink_result& result) -> std::string {
  std::string out   = "{\"result\":{";
  bool        first = true;
  append_kv(out, first, "Name", result.name);
  append_kv(out, first, "Kind", manifest::kind_name(result.kind));
  out.append(",\"Removed\":[");
  for (std::size_t i = 0; i < result.removed.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_target_record(out, result.removed[i]);
  }
  out.append("],\"PurgedFile\":");
  append_json_string(out, result.purged_file);
  if (!result.skipped.empty()) {
    out.append(",\"Skipped\":[");
    for (std::size_t i = 0; i < result.skipped.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      append_target_record(out, result.skipped[i]);
    }
    out.push_back(']');
  }
  out.append("}}\n");
  return out;
}

auto unlink_text(const link::unlink_result& result) -> std::string {
  std::string out = std::format("{} ({})\n", result.name, manifest::kind_name(result.kind));
  for (const auto& rec : result.removed) {
    out.append(std::format("  {}  removed  <-  {}\n", pad(rec.vendor, 7), rec.target_path));
  }
  for (const auto& rec : result.skipped) {
    out.append(std::format("  {}  skipped: not owned  <-  {}\n", pad(rec.vendor, 7), rec.target_path));
  }
  if (!result.purged_file.empty()) {
    out.append(std::format("  purged source file: {}\n", result.purged_file));
  }
  return out;
}

auto unlink_none_text(std::string_view name) -> std::string {
  return std::format("no installs found for \"{}\" (already unlinked, or no such name)\n", name);
}

namespace {

auto append_import_record(std::string& out, const import_::record& rec) -> void {
  out.push_back('{');
  bool first = true;
  append_kv(out, first, "Name", rec.name);
  append_kv(out, first, "SourcePath", rec.source_path);
  append_kv(out, first, "TargetPath", rec.target_path);
  append_kv(out, first, "Action", rec.action);
  // Unconditional, unlike the link records' omitted-when-empty warning: this
  // call site passes no emit_null_optional_fields option at all.
  append_kv(out, first, "Reason", rec.reason);
  out.push_back('}');
}

} // namespace

auto import_json(const import_::result& value) -> std::string {
  std::string out = "{\"Imported\":[";
  for (std::size_t i = 0; i < value.imported.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_import_record(out, value.imported[i]);
  }
  out.append("],\"Skipped\":[");
  for (std::size_t i = 0; i < value.skipped.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_import_record(out, value.skipped[i]);
  }
  out.append("],\"Warnings\":[");
  for (std::size_t i = 0; i < value.warnings.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    bool first = true;
    append_kv(out, first, "Name", value.warnings[i].name);
    append_kv(out, first, "Field", value.warnings[i].field);
    append_kv(out, first, "Message", value.warnings[i].message);
    out.push_back('}');
  }
  out.append("]}\n");
  return out;
}

auto import_text(const import_::result& value) -> std::string {
  if (value.imported.empty() && value.skipped.empty()) {
    return "no files matched for import\n";
  }
  std::string out;
  for (const auto& rec : value.imported) {
    out.append(std::format("{}  {}  <-  {}\n", pad(rec.name, 12), pad(rec.action, 12), rec.source_path));
  }
  for (const auto& rec : value.skipped) {
    out.append(std::format("{}  skipped       reason: {}\n", pad(rec.name, 12), rec.reason));
  }
  for (const auto& warn : value.warnings) {
    out.append(std::format("warning [{}.{}] {}\n", warn.name, warn.field, warn.message));
  }
  // Says "imported" even under --dry-run, where every row above reads
  // `would-import`. The Zig handler takes `dry_run` and discards it.
  out.append(std::format("\nimported {} file(s); skipped {}\n", value.imported.size(), value.skipped.size()));
  return out;
}

namespace {

auto append_migrate_record(std::string& out, const manifest::migrate_record& rec) -> void {
  out.push_back('{');
  bool first = true;
  append_kv(out, first, "Name", rec.name);
  append_kv(out, first, "OldPath", rec.old_path);
  append_kv(out, first, "NewPath", rec.new_path);
  append_kv(out, first, "Reason", rec.reason);
  out.push_back('}');
}

} // namespace

auto migrate_json(const manifest::migrate_result& value) -> std::string {
  std::string out = "{\"Migrated\":[";
  for (std::size_t i = 0; i < value.migrated.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_migrate_record(out, value.migrated[i]);
  }
  out.append("],\"Skipped\":[");
  for (std::size_t i = 0; i < value.skipped.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    append_migrate_record(out, value.skipped[i]);
  }
  out.append("]}\n");
  return out;
}

auto migrate_text(const manifest::migrate_result& value, bool dry_run) -> std::string {
  if (value.migrated.empty() && value.skipped.empty()) {
    return "migrate: no legacy flat skills found; sandbox is already dir-shape\n";
  }
  const std::string_view verb = dry_run ? "would migrate" : "migrated";
  std::string            out;
  for (const auto& rec : value.migrated) {
    out.append(std::format("{}  {}  {} -> {}\n", pad(rec.name, 20), verb, rec.old_path, rec.new_path));
  }
  for (const auto& rec : value.skipped) {
    out.append(std::format("{}  skipped       {}\n", pad(rec.name, 20), rec.reason));
  }
  out.append("\n");
  out.append(std::format("done: {} {} skill(s); skipped {}\n", verb, value.migrated.size(), value.skipped.size()));
  return out;
}

auto reconcile_json(std::span<const link::reconcile_action> actions) -> std::string {
  std::string out;
  for (const auto& action : actions) {
    out.push_back('{');
    bool first = true;
    // snake_case, unlike every sibling leaf: this one is rendered from an
    // anonymous struct rather than a Go-shaped mirror.
    append_kv(out, first, "name", action.name);
    append_kv(out, first, "kind", manifest::kind_name(action.kind));
    append_kv(out, first, "reason", action.reason);
    append_kv(out, first, "source_path", action.source_path);
    out.append(",\"removed_targets\":[");
    for (std::size_t i = 0; i < action.removed_targets.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      append_json_string(out, action.removed_targets[i]);
    }
    out.append("]}\n");
  }
  return out;
}

auto reconcile_text(std::span<const link::reconcile_action> actions, bool dry_run) -> std::string {
  if (actions.empty()) {
    return "reconcile: manifest already consistent with the filesystem\n";
  }
  std::string out;
  std::size_t changes = 0;
  for (const auto& action : actions) {
    if (action.reason == "not-owned") {
      out.append(std::format("{} ({}) - skipped: not owned; left {} path(s) in place\n", action.name,
                             manifest::kind_name(action.kind), action.removed_targets.size()));
      for (const auto& path : action.removed_targets) {
        out.append(std::format("  {}\n", path));
      }
      continue;
    }
    if (action.reason == "write-failed") {
      out.append(std::format("error: could not write {} path(s); not recorded\n", action.removed_targets.size()));
      for (const auto& path : action.removed_targets) {
        out.append(std::format("  {}\n", path));
      }
      continue;
    }
    const bool             removal = action.reason == "source-missing" || action.reason == "legacy";
    const std::string_view verb    = removal ? (dry_run ? "would remove" : "removed") : (dry_run ? "would write" : "wrote");
    out.append(std::format("{} ({}) - {}; {} {} path(s)\n", action.name, manifest::kind_name(action.kind), action.reason, verb,
                           action.removed_targets.size()));
    for (const auto& path : action.removed_targets) {
      out.append(std::format("  {}\n", path));
    }
    changes += action.removed_targets.size();
  }
  if (dry_run) {
    out.append(std::format("\ndry-run: {} change(s) would be made\n", changes));
  } else {
    out.append(std::format("\ndone: {} change(s)\n", changes));
  }
  return out;
}

auto no_sources_text(std::string_view sandbox_root) -> std::string {
  return std::format("no sandbox sources to link under {}\n", sandbox_root);
}

auto no_such_source_error(std::string_view name, std::string_view sandbox_root) -> std::string {
  return std::format("error: no sandbox source named \"{}\" under {}\n", name, sandbox_root);
}

auto invalid_kind_error(std::string_view raw) -> std::string {
  return std::format("error: --kind must be skill or agent, got \"{}\"\n", raw);
}

auto reconcile_takes_no_positional_error() -> std::string {
  return "error: --reconcile takes no positional arguments\n";
}

} // namespace planar::engine::local::render
