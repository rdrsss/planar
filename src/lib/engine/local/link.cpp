/// @file link.cpp
/// @brief Implementation of `planar.engine.local.link` (plan 996, task 6109).
/// See link.cppm for the vendor layouts, the shadow rule, and the three status
/// words.

module planar.engine.local.link;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::link {

namespace {

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  // A DANGLING symlink does not "exist" but is very much there, and must count
  // as present so it gets replaced rather than double-created.
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

/// @brief The symlink's target, or nullopt when `path` is not a symlink.
auto read_link(const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
  std::error_code ec;
  auto            target = std::filesystem::read_symlink(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return target;
}

/// @brief `<home>/.claude/commands` etc. — nullopt for an unknown vendor.
auto skill_dir(const std::filesystem::path& home_dir, std::string_view vendor) -> std::optional<std::filesystem::path> {
  if (vendor == "claude") {
    return home_dir / ".claude" / "commands";
  }
  if (vendor == "codex") {
    return home_dir / ".codex" / "skills";
  }
  if (vendor == "copilot") {
    return home_dir / ".copilot" / "skills";
  }
  return std::nullopt;
}

/// @brief Whether a vendor links the whole skill DIRECTORY rather than one file.
auto is_dir_symlink_vendor(std::string_view vendor) -> bool {
  return vendor == "codex" || vendor == "copilot";
}

auto layout_for_record(manifest::kind row_kind, std::string_view vendor) -> layout {
  if (row_kind == manifest::kind::skill && is_dir_symlink_vendor(vendor)) {
    return layout::dir_symlink;
  }
  return layout::flat;
}

auto remove_path_if_exists(const std::filesystem::path& path) -> void {
  std::error_code ec;
  // remove() unlinks a symlink without following it, which is what we want:
  // deleting through a codex directory link must never delete the sandbox
  // source it points at.
  if (std::filesystem::remove(path, ec)) {
    return;
  }
  std::filesystem::remove_all(path, ec);
}

/// @brief Whether `path` is a REAL directory, not a symlink pointing at one.
auto is_real_directory(const std::filesystem::path& path) -> bool {
  if (read_link(path)) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

auto rename_into_place(const std::filesystem::path& tmp, const std::filesystem::path& target) -> bool {
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  if (!ec) {
    return true;
  }
  // Rename onto a NON-EMPTY directory fails on every platform. Clear the way and
  // retry — and clear it the right way: remove_all() on a symlink would follow
  // nothing (it unlinks the link), while remove() on a real directory tree
  // fails, so the two cases genuinely need different calls.
  if (is_real_directory(target)) {
    std::filesystem::remove_all(target, ec);
  } else {
    std::filesystem::remove(target, ec);
  }
  ec.clear();
  std::filesystem::rename(tmp, target, ec);
  return !ec;
}

auto copy_file_bytes(const std::filesystem::path& src, const std::filesystem::path& dst) -> bool {
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    return false;
  }
  std::error_code ec;
  if (dst.has_parent_path()) {
    std::filesystem::create_directories(dst.parent_path(), ec);
  }
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << in.rdbuf();
  out.flush();
  return static_cast<bool>(out) && !in.bad();
}

/// @brief Recursive copy preserving symlinks AS symlinks.
///
/// `copy_symlinks` rather than following them: a skill directory may legitimately
/// contain a link, and resolving it during a copy would silently change what the
/// installed skill contains.
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
    const auto child_dst = dst / entry.path().filename();
    if (const auto target = read_link(entry.path())) {
      std::error_code lec;
      std::filesystem::create_symlink(*target, child_dst, lec);
      if (lec) {
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

enum class existing_state { absent, unchanged, changed };

auto classify_current(const std::filesystem::path& target_path, std::string_view source_path) -> existing_state {
  if (const auto current = read_link(target_path)) {
    return current->string() == source_path ? existing_state::unchanged : existing_state::changed;
  }
  return path_exists(target_path) ? existing_state::changed : existing_state::absent;
}

struct install_result {
  manifest::mode mode = manifest::mode::symlink;
  std::string    action;
};

/// @brief Materialise one install, symlink-first with a copy fallback.
auto install_one(std::string_view source_path, const std::filesystem::path& target_path, layout target_layout, bool force_copy)
    -> install_result {
  const auto current = classify_current(target_path, source_path);
  if (current == existing_state::unchanged && !force_copy) {
    return {manifest::mode::symlink, "unchanged"};
  }
  const std::string action = current == existing_state::absent ? "created" : "updated";

  const std::filesystem::path tmp{target_path.string() + ".planar-tmp"};
  std::error_code             ec;
  std::filesystem::remove(tmp, ec);
  ec.clear();
  std::filesystem::remove_all(tmp, ec);

  if (!force_copy) {
    std::error_code lec;
    std::filesystem::create_symlink(std::filesystem::path{source_path}, tmp, lec);
    if (!lec) {
      rename_into_place(tmp, target_path);
      return {manifest::mode::symlink, action};
    }
  }

  const bool ok = target_layout == layout::dir_symlink ? copy_tree(source_path, tmp) : copy_file_bytes(source_path, tmp);
  if (ok) {
    rename_into_place(tmp, target_path);
  }
  return {manifest::mode::copy, action};
}

auto find_manifest_entry(const manifest::link_manifest& value, std::string_view name) -> std::optional<std::size_t> {
  for (std::size_t i = 0; i < value.entries.size(); ++i) {
    if (value.entries[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

/// @brief Convert install records into a manifest entry.
///
/// `skipped` and `dry-run` records are DROPPED here. That single filter is what
/// makes `--vendor` narrow the recorded set rather than merge into it, and what
/// keeps `--dry-run` from recording links it never made.
auto to_manifest_entry(std::string_view name, std::string_view source_path, std::span<const target_record> records)
    -> manifest::manifest_entry {
  manifest::manifest_entry entry{std::string{name}, std::string{source_path}, {}};
  for (const auto& rec : records) {
    if (rec.action == "skipped" || rec.action == "dry-run") {
      continue;
    }
    entry.links.push_back(
        {rec.vendor, rec.target_path, rec.source_path, rec.mode.value_or(manifest::mode::symlink), rec.linked_at});
  }
  return entry;
}

auto put_manifest_entry(manifest::link_manifest& value, std::string_view name, std::string_view source_path,
                        std::span<const target_record> records) -> void {
  auto entry = to_manifest_entry(name, source_path, records);
  if (const auto idx = find_manifest_entry(value, name)) {
    value.entries[*idx] = std::move(entry);
  } else {
    value.entries.push_back(std::move(entry));
  }
  value.version = 1;
}

} // namespace

auto format_utc_stamp(std::int64_t epoch_seconds) -> std::string {
  if (epoch_seconds < 0) {
    return {};
  }
  const std::chrono::sys_seconds    tp{std::chrono::seconds{epoch_seconds}};
  const auto                        days = std::chrono::floor<std::chrono::days>(tp);
  const std::chrono::year_month_day ymd{days};
  const std::chrono::hh_mm_ss       hms{tp - days};
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                     static_cast<unsigned>(ymd.day()), hms.hours().count(), hms.minutes().count(), hms.seconds().count());
}

auto utc_now_stamp() -> std::string {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return format_utc_stamp(now.time_since_epoch().count());
}

auto targets(const std::filesystem::path& home_dir, const std::filesystem::path& source_dir, std::string_view name,
             manifest::kind source_kind, bool shadow, std::span<const std::string> vendors) -> std::vector<target> {
  std::vector<target> out;
  if (home_dir.empty() || name.empty()) {
    return out;
  }
  const auto basename = shadow ? std::string{name} : std::format("local-{}", name);
  const auto filename = std::format("{}.md", basename);

  if (source_kind == manifest::kind::agent) {
    // ONE target, no vendor fan-out, and an empty link_target that link() reads
    // as "point at the source file itself". Frontmatter `vendors:` is ignored
    // entirely for agents — oracle-confirmed.
    out.push_back({"agents", layout::flat, (home_dir / ".planar" / "agents" / filename).string(), ""});
    return out;
  }

  if (source_dir.empty()) {
    return out;
  }
  for (const auto& vendor : vendors) {
    const auto install_dir = skill_dir(home_dir, vendor);
    if (!install_dir) {
      continue;
    }
    if (is_dir_symlink_vendor(vendor)) {
      out.push_back({vendor, layout::dir_symlink, (*install_dir / basename).string(), source_dir.string()});
    } else {
      out.push_back({vendor, layout::flat, (*install_dir / filename).string(), (source_dir / "SKILL.md").string()});
    }
  }
  std::ranges::sort(out, [](const target& lhs, const target& rhs) { return lhs.vendor < rhs.vendor; });
  return out;
}

auto link(const manifest::sandbox_file& file, const link_options& options) -> link_result {
  link_result result{file.name, file.kind, {}};
  if (options.home_dir.empty()) {
    return result;
  }
  const std::filesystem::path source_path{file.source_path};
  const auto                  source_dir = source_path.has_parent_path() ? source_path.parent_path() : std::filesystem::path{};
  const auto                  vendors    = manifest::resolved_vendors(file.frontmatter);
  const auto                  tgs = targets(options.home_dir, source_dir, file.name, file.kind, file.frontmatter.shadow, vendors);

  for (const auto& tgt : tgs) {
    target_record rec{
        tgt.vendor, tgt.target_path, tgt.link_target.empty() ? file.source_path : tgt.link_target, std::nullopt, "", "", ""};

    if (options.vendor_filter && *options.vendor_filter != rec.vendor) {
      rec.action = "skipped";
      result.records.push_back(std::move(rec));
      continue;
    }

    if (file.frontmatter.shadow) {
      rec.warning = std::format("shadow: linked as \"{}\" (no local- prefix); any canonical install with this name is replaced",
                                std::filesystem::path{rec.target_path}.filename().string());
    }

    if (options.dry_run) {
      rec.action = "dry-run";
      rec.mode   = options.force_copy ? manifest::mode::copy : manifest::mode::symlink;
      result.records.push_back(std::move(rec));
      continue;
    }

    const std::filesystem::path target_path{rec.target_path};
    std::error_code             ec;
    if (target_path.has_parent_path()) {
      std::filesystem::create_directories(target_path.parent_path(), ec);
    }

    const auto installed = install_one(rec.source_path, target_path, tgt.layout, options.force_copy);
    rec.mode             = installed.mode;
    rec.action           = installed.action;
    rec.linked_at        = options.now;
    if (installed.mode == manifest::mode::copy && rec.warning.empty()) {
      // Only when there is no shadow warning already: the Zig original checks
      // `rec.warning.len == 0`, so a shadowed skill that ALSO fell back to a copy
      // reports only the shadow warning. Preserved.
      rec.warning = "copy fallback: edits to source require re-running `planar local link`";
    }
    result.records.push_back(std::move(rec));
  }

  if (!options.dry_run) {
    auto loaded = manifest::load_manifest(options.home_dir, file.kind);
    if (loaded) {
      put_manifest_entry(*loaded, file.name, file.source_path, result.records);
      manifest::save_manifest(options.home_dir, file.kind, *loaded);
    }
  }
  return result;
}

auto unlink(std::string_view name, manifest::kind unlink_kind, const unlink_options& options) -> std::optional<unlink_result> {
  if (options.home_dir.empty() || name.empty()) {
    return std::nullopt;
  }
  auto loaded = manifest::load_manifest(options.home_dir, unlink_kind);
  if (!loaded) {
    return std::nullopt;
  }

  unlink_result result{std::string{name}, unlink_kind, {}, ""};

  if (const auto idx = find_manifest_entry(*loaded, name)) {
    for (const auto& row : loaded->entries[*idx].links) {
      remove_path_if_exists(std::filesystem::path{row.target_path});
      result.removed.push_back({row.vendor, row.target_path, row.source_path, row.mode, "removed", "", row.linked_at});
    }
    loaded->entries.erase(loaded->entries.begin() + static_cast<std::ptrdiff_t>(*idx));
    loaded->version = 1;
    manifest::save_manifest(options.home_dir, unlink_kind, *loaded);
  }

  if (options.purge) {
    std::error_code ec;
    if (unlink_kind == manifest::kind::skill) {
      const auto dir = options.home_dir / ".planar" / "local" / "skills" / std::string{name};
      std::filesystem::remove_all(dir, ec);
      result.purged_file = dir.string();
    } else {
      const auto path = options.home_dir / ".planar" / "local" / "agents" / std::format("{}.md", name);
      std::filesystem::remove(path, ec);
      result.purged_file = path.string();
    }
    // The error is DISCARDED, matching the Zig original's `catch {}`. So
    // `purged_file` means "the path purge targeted", not "the path that was
    // deleted" — a name that never existed still reports one.
  }
  return result;
}

auto classify_existing(const manifest::manifest_record& row) -> std::string_view {
  const std::filesystem::path target{row.target_path};
  const auto                  current = read_link(target);
  if (!current) {
    // NOT a symlink. A real file an operator dropped in by hand therefore
    // reports `live`. That is the oracle's behavior and it is pinned rather
    // than corrected.
    return path_exists(target) ? "live" : "missing";
  }
  if (current->string() != row.source_path) {
    return "broken";
  }
  if (!path_exists(std::filesystem::path{row.source_path})) {
    return "broken";
  }
  return "live";
}

auto list(const std::filesystem::path& home_dir) -> std::optional<std::vector<list_record>> {
  if (home_dir.empty()) {
    return std::nullopt;
  }
  std::vector<list_record> out;
  for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
    const auto loaded = manifest::load_manifest(home_dir, row_kind);
    if (!loaded) {
      return std::nullopt;
    }
    for (const auto& entry : loaded->entries) {
      for (const auto& row : entry.links) {
        out.push_back(
            {entry.name,
             row_kind,
             {row.vendor, row.target_path, row.source_path, row.mode, std::string{classify_existing(row)}, "", row.linked_at}});
      }
    }
  }
  std::ranges::sort(out, [](const list_record& lhs, const list_record& rhs) {
    if (lhs.kind != rhs.kind) {
      return lhs.kind < rhs.kind;
    }
    if (lhs.name != rhs.name) {
      return lhs.name < rhs.name;
    }
    return lhs.record.vendor < rhs.record.vendor;
  });
  return out;
}

auto reconcile(const reconcile_options& options) -> std::optional<std::vector<reconcile_action>> {
  if (options.home_dir.empty()) {
    return std::nullopt;
  }
  std::vector<reconcile_action> actions;

  for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
    auto loaded = manifest::load_manifest(options.home_dir, row_kind);
    if (!loaded) {
      return std::nullopt;
    }
    std::vector<manifest::manifest_entry> keep;

    for (const auto& entry : loaded->entries) {
      if (path_exists(std::filesystem::path{entry.source_path})) {
        std::vector<manifest::manifest_record> links;
        std::vector<std::string>               repaired;

        for (const auto& row : entry.links) {
          auto next = row;
          if (classify_existing(row) != "live") {
            repaired.push_back(row.target_path);
            if (!options.dry_run) {
              const std::filesystem::path target{row.target_path};
              std::error_code             ec;
              if (target.has_parent_path()) {
                std::filesystem::create_directories(target.parent_path(), ec);
              }
              const auto installed = install_one(row.source_path, target, layout_for_record(row_kind, row.vendor), false);
              next.mode            = installed.mode;
              next.linked_at       = options.now;
            }
          }
          links.push_back(std::move(next));
        }

        if (!repaired.empty()) {
          // `target-missing` REPAIRS rather than removes, yet the paths land in
          // `removed_targets` and the CLI prints them as "removed". Preserved —
          // see link.cppm and render.cppm.
          actions.push_back({entry.name, row_kind, "target-missing", entry.source_path, std::move(repaired)});
        }
        keep.push_back({entry.name, entry.source_path, std::move(links)});
        continue;
      }

      std::vector<std::string> removed;
      for (const auto& row : entry.links) {
        if (!options.dry_run) {
          remove_path_if_exists(std::filesystem::path{row.target_path});
        }
        removed.push_back(row.target_path);
      }
      actions.push_back({entry.name, row_kind, "source-missing", entry.source_path, std::move(removed)});
    }

    if (!options.dry_run) {
      manifest::save_manifest(options.home_dir, row_kind, manifest::link_manifest{1, std::move(keep)});
    }
  }
  return actions;
}

} // namespace planar::engine::local::link
