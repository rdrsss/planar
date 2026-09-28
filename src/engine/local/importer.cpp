/// @file importer.cpp
/// @brief Implementation of `planar.engine.local.import` (plan 996, task 6109).
/// See importer.cppm for the three source shapes and the collision rules.

module planar.engine.local.importer;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::import_ {

namespace {

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

auto is_dir(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

/// @brief One resolved source before it is parsed or copied.
struct source_entry {
  std::string name;             ///< The entry's name.
  std::string root_source_path; ///< The directory (shape 1/2-dir) or the file itself.
  std::string file_source_path; ///< Always the `.md` file to parse.
  bool        is_dir = false;   ///< Whether root_source_path is a directory to copy wholesale.
};

auto collect_from_collection(const std::filesystem::path& dir, manifest::kind kind) -> std::vector<source_entry> {
  std::vector<source_entry>           out;
  std::error_code                     ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    return out;
  }
  for (const auto& entry : it) {
    const auto name = entry.path().filename().string();
    if (name.empty() || name.front() == '.') {
      continue;
    }
    if (name == manifest::manifest_filename) {
      // Redundant with the dot-prefix skip above (the filename starts with a
      // dot), but the Zig original checks both and the redundancy is harmless.
      // Kept so a future rename of the manifest file cannot silently start
      // importing it.
      continue;
    }
    const auto full = dir / name;

    std::error_code dec;
    if (std::filesystem::is_directory(full, dec)) {
      if (kind != manifest::kind::skill) {
        continue;
      }
      const auto skill_md = full / "SKILL.md";
      if (!path_exists(skill_md)) {
        continue;
      }
      out.push_back({name, full.string(), skill_md.string(), true});
      continue;
    }

    if (!name.ends_with(".md")) {
      continue;
    }
    out.push_back({name.substr(0, name.size() - 3), full.string(), full.string(), false});
  }
  std::ranges::sort(out, [](const source_entry& lhs, const source_entry& rhs) { return lhs.name < rhs.name; });
  return out;
}

auto collect_entries(const std::filesystem::path& src, manifest::kind kind)
    -> std::expected<std::vector<source_entry>, import_error> {
  if (is_dir(src)) {
    if (kind == manifest::kind::skill) {
      const auto skill_md = src / "SKILL.md";
      if (path_exists(skill_md)) {
        // Shape 1, checked FIRST: a directory holding SKILL.md is one skill
        // named for the directory, not a collection.
        return std::vector<source_entry>{{src.filename().string(), src.string(), skill_md.string(), true}};
      }
    }
    return collect_from_collection(src, kind);
  }

  const auto base = src.filename().string();
  if (!base.ends_with(".md")) {
    return std::unexpected(import_error::invalid_input);
  }
  return std::vector<source_entry>{{base.substr(0, base.size() - 3), src.string(), src.string(), false}};
}

auto copy_file_bytes(const std::filesystem::path& src, const std::filesystem::path& dst) -> bool {
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) {
    return false;
  }
  const auto body = buf.str();

  // Write to `<dst>.planar-tmp` and rename, so an interrupted import cannot
  // leave a truncated skill where a working one was.
  const std::filesystem::path tmp{dst.string() + ".planar-tmp"};
  std::error_code             ec;
  if (tmp.has_parent_path()) {
    std::filesystem::create_directories(tmp.parent_path(), ec);
  }
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.flush();
    if (!out) {
      return false;
    }
  }
  ec.clear();
  std::filesystem::rename(tmp, dst, ec);
  return !ec;
}

auto copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(dst, ec);
  if (ec) {
    return false;
  }
  std::filesystem::directory_iterator it(src, ec);
  if (ec) {
    return false;
  }
  for (const auto& entry : it) {
    const auto      child_dst = dst / entry.path().filename();
    std::error_code lec;
    auto            target = std::filesystem::read_symlink(entry.path(), lec);
    if (!lec) {
      // Symlinks are recreated AS symlinks, not resolved — an imported skill
      // keeps whatever indirection its author built into it.
      std::error_code cec;
      std::filesystem::create_symlink(target, child_dst, cec);
      if (cec) {
        return false;
      }
      continue;
    }
    std::error_code dec;
    if (std::filesystem::is_directory(entry.path(), dec)) {
      if (!copy_tree(entry.path(), child_dst)) {
        return false;
      }
      continue;
    }
    if (!copy_file_bytes(entry.path(), child_dst)) {
      return false;
    }
  }
  return true;
}

auto place_entry(const source_entry& entry, const std::filesystem::path& target, manifest::kind kind) -> bool {
  if (kind == manifest::kind::agent) {
    return copy_file_bytes(entry.file_source_path, target);
  }
  if (!target.has_parent_path()) {
    return false;
  }
  const auto dest_dir = target.parent_path();
  if (entry.is_dir) {
    // Destructive by design under --force: the destination tree is REMOVED
    // before the copy, so files in the old skill and not the new one are gone
    // rather than merged.
    std::error_code ec;
    std::filesystem::remove_all(dest_dir, ec);
    return copy_tree(entry.root_source_path, dest_dir);
  }
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  return copy_file_bytes(entry.file_source_path, target);
}

} // namespace

auto dest_path(const std::filesystem::path& dest_root, std::string_view name, manifest::kind entry_kind)
    -> std::filesystem::path {
  if (entry_kind == manifest::kind::skill) {
    // A flat source file is PROMOTED into directory shape on the way in, so
    // nothing imported ever needs `local migrate`.
    return dest_root / std::string{name} / "SKILL.md";
  }
  return dest_root / std::format("{}.md", name);
}

auto import_sources(const options& opts) -> std::expected<result, import_error> {
  if (opts.home_dir.empty() || opts.source_path.empty()) {
    return std::unexpected(import_error::invalid_input);
  }
  auto entries = collect_entries(opts.source_path, opts.kind);
  if (!entries) {
    return std::unexpected(entries.error());
  }
  if (entries->empty()) {
    // Zero importable entries is an ERROR here, unlike `local link`'s cheerful
    // exit 0 on an empty sandbox. The two leaves genuinely disagree.
    return std::unexpected(import_error::not_found);
  }

  const auto dest_root = opts.home_dir / ".planar" / "local" / std::string{manifest::kind_dir(opts.kind)};
  if (!opts.dry_run) {
    std::error_code ec;
    std::filesystem::create_directories(dest_root, ec);
  }

  result out;
  for (const auto& entry : *entries) {
    auto parsed = manifest::parse_file(std::filesystem::path{entry.file_source_path}, opts.kind);
    if (!parsed) {
      out.skipped.push_back(
          {entry.name, entry.file_source_path, "", "skipped", std::string{manifest::parse_error_reason(parsed.error())}});
      continue;
    }

    for (const auto& issue : manifest::lint(parsed->frontmatter, entry.name)) {
      if (issue.severity != manifest::lint_severity::warning) {
        continue;
      }
      out.warnings.push_back({entry.name, issue.field, issue.message});
    }

    const auto target = dest_path(dest_root, entry.name, opts.kind);
    const auto exists = path_exists(target);

    if (exists && !opts.force) {
      // Note the warnings above are kept even though this entry is skipped:
      // the operator still wants to know the source they tried to import has an
      // empty description.
      out.skipped.push_back({entry.name, entry.file_source_path, target.string(), "skipped", "name-collision"});
      continue;
    }

    const std::string action = opts.dry_run ? "would-import" : (exists ? "overwrote" : "imported");
    if (!opts.dry_run) {
      place_entry(entry, target, opts.kind);
    }
    out.imported.push_back({entry.name, entry.file_source_path, target.string(), action, ""});
  }

  const auto by_name = [](const record& lhs, const record& rhs) { return lhs.name < rhs.name; };
  std::ranges::sort(out.imported, by_name);
  std::ranges::sort(out.skipped, by_name);
  std::ranges::sort(out.warnings, [](const warning& lhs, const warning& rhs) { return lhs.name < rhs.name; });
  return out;
}

} // namespace planar::engine::local::import_
