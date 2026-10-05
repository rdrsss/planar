/// @file manifest.cpp
/// @brief Implementation of `planar.engine.local.manifest` (plan 996, task
/// 6109). See manifest.cppm for the HOME-safety mechanism, the frontmatter
/// grammar as implemented, and the frozen error identifiers.

module;

#include <glaze/glaze.hpp>

module planar.engine.local.manifest;

import std;
import planar.json_text;

namespace planar::engine::local::manifest {

namespace {

constexpr std::string_view k_open_fence  = "---\n";
constexpr std::string_view k_close_fence = "\n---\n";
constexpr std::string_view k_eof_fence   = "\n---";
constexpr std::string_view k_trim_chars  = " \t";
constexpr std::string_view k_line_trim   = " \t\r";

/// @brief Trim `chars` from both ends.
auto trim(std::string_view text, std::string_view chars) -> std::string_view {
  const auto first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(chars) - first + 1);
}

/// @brief Strip ONE matching pair of surrounding single or double quotes.
///
/// Deliberately not a YAML string parser: `"a"b"` loses only the outermost
/// pair, and `'a"` (mismatched) is left alone entirely. That is the Zig
/// original's behavior and what the oracle emits.
auto trim_quotes(std::string_view text) -> std::string_view {
  if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

/// @brief Cut a `#` comment off a vendors-list item.
///
/// Quote-aware: a `#` inside single or double quotes is literal. Applied ONLY
/// to `vendors:` items, never to the scalar keys — `description: x # y` keeps
/// its `# y`, which is asserted in the tests because it looks like a bug and
/// is not one.
auto strip_inline_comment(std::string_view text) -> std::string_view {
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"' && !in_single) {
      in_double = !in_double;
      continue;
    }
    if (c == '\'' && !in_double) {
      in_single = !in_single;
      continue;
    }
    if (c == '#' && !in_single && !in_double) {
      return text.substr(0, i);
    }
  }
  return text;
}

auto is_known_vendor(std::string_view value) -> bool {
  return std::ranges::find(all_vendors, value) != all_vendors.end();
}

/// @brief Read a whole file, or nullopt when it cannot be read.
auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) && !std::filesystem::is_symlink(path, ec)) {
    // A symlink to a regular file reports is_regular_file() true through the
    // follow, so this only rejects directories, sockets and absent paths.
    if (!std::filesystem::exists(path, ec)) {
      return std::nullopt;
    }
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  return buf.str();
}

/// @brief Write `data` to `path`, creating parents.
auto write_file(const std::filesystem::path& path, std::string_view data) -> bool {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.flush();
  return static_cast<bool>(out);
}

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  // exists() follows symlinks and therefore reports FALSE for a dangling one.
  // symlink_status() catches that case, which matters: a broken install must
  // still count as "something is there" so it gets replaced rather than
  // silently double-created.
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

/// @brief Absolute form of `path`, or `path` unchanged when that fails.
///
/// The Zig original does the same fall-back (`catch allocator.dupe(path)`), so
/// an unresolvable path yields a relative `source_path` rather than an error.
auto to_absolute(const std::filesystem::path& path) -> std::filesystem::path {
  std::error_code ec;
  auto            abs = std::filesystem::absolute(path, ec);
  if (ec) {
    return path;
  }
  return abs.lexically_normal();
}

/// @brief The frontmatter key/value reader.
///
/// See manifest.cppm for the grammar. Everything here is line-oriented over
/// '\n' with each line trimmed of " \t\r", which is why CRLF files parse
/// identically to LF ones with no normalization pass.
auto parse_frontmatter_block(std::string_view raw) -> frontmatter {
  frontmatter out;
  bool        mode_vendors = false;
  bool        mode_planar  = false; // inside the `planar:` map of the namespaced form

  std::size_t pos = 0;
  while (pos <= raw.size()) {
    const auto next = raw.find('\n', pos);
    const auto seg  = raw.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    const auto line = trim(seg, k_line_trim);
    if (next == std::string_view::npos) {
      pos = raw.size() + 1;
    } else {
      pos = next + 1;
    }

    if (line.empty()) {
      // NOTE: a blank line does NOT end a vendors block list, because the
      // continue happens before `mode_vendors` is cleared.
      continue;
    }

    if (mode_vendors && line.front() == '-') {
      const auto item = trim(strip_inline_comment(line.substr(1)), k_trim_chars);
      if (!item.empty()) {
        out.vendors.emplace_back(trim_quotes(item));
      }
      continue;
    }
    mode_vendors = false;

    // An indented line belongs to the block opened above it; a column-0 line closes it.
    const bool indented = !seg.empty() && (seg.front() == ' ' || seg.front() == '\t');
    if (!indented) {
      mode_planar = false;
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const auto key = trim(line.substr(0, colon), k_trim_chars);
    const auto val = trim(line.substr(colon + 1), k_trim_chars);

    if (mode_planar && indented) {
      // Namespaced form: only `planar.kind` is read; `planar.slug` is not used because a local
      // source's name comes from its filename.
      if (key == "kind") {
        out.kind = trim_quotes(val);
      }
      continue;
    }
    if (!indented && key == "planar" && val.empty()) {
      mode_planar = true;
      continue;
    }

    if (key == "description") {
      out.description = trim_quotes(val);
    } else if (key == "argument-hint") {
      out.argument_hint = trim_quotes(val);
    } else if (key == "tier") {
      out.tier = trim_quotes(val);
    } else if (key == "model") {
      out.model = trim_quotes(val);
    } else if (key == "kind") {
      // Deprecated: a top-level `kind:` is the pre-namespacing form. It is still accepted because
      // operator-local files are not ours to rename; Planar's own agents use `planar.kind`.
      out.kind = trim_quotes(val);
    } else if (key == "shadow") {
      out.shadow = trim_quotes(val) == "true";
    } else if (key == "vendors") {
      if (val.empty()) {
        mode_vendors = true;
        continue;
      }
      if (val.front() == '[' && val.size() >= 2 && val.back() == ']') {
        const auto  inner = val.substr(1, val.size() - 2);
        std::size_t ipos  = 0;
        while (ipos <= inner.size()) {
          const auto icomma = inner.find(',', ipos);
          const auto piece  = inner.substr(ipos, icomma == std::string_view::npos ? std::string_view::npos : icomma - ipos);
          const auto item   = trim(strip_inline_comment(piece), k_trim_chars);
          if (!item.empty()) {
            out.vendors.emplace_back(trim_quotes(item));
          }
          if (icomma == std::string_view::npos) {
            break;
          }
          ipos = icomma + 1;
        }
      } else {
        const auto item = trim(strip_inline_comment(val), k_trim_chars);
        if (!item.empty()) {
          out.vendors.emplace_back(trim_quotes(item));
        }
      }
    }
  }
  return out;
}

} // namespace

// Glaze's compile-time reflection (glaze/reflection/get_name.hpp) needs its
// reflected types to have EXTERNAL LINKAGE — it materialises a variable
// template keyed on the type, which cannot be defined for a type declared in
// an anonymous namespace. So these three sit in a named detail namespace
// rather than the file-local one every other helper here uses. `core/
// vendor_probe.cpp` documents the same constraint.
namespace wire {

/// @brief The Glaze-facing mirror of manifest_record.
///
/// Separate from the exported type because the file carries `mode` as the
/// STRING `symlink` / `copy` while the exported struct carries an enum. Read as
/// a plain string and translated in parse_manifest(), rather than teaching
/// Glaze an enum mapping that would then have to stay in sync with mode_name().
struct record {
  std::string vendor;      ///< Vendor name as written.
  std::string target_path; ///< Install path as written.
  std::string source_path; ///< Source path as written.
  std::string mode;        ///< `symlink` or `copy`; anything else reads as symlink.
  std::string linked_at;   ///< Timestamp as written.
};

/// @brief The Glaze-facing mirror of manifest_entry.
struct entry {
  std::string         name;        ///< Source name as written.
  std::string         source_path; ///< Source path as written.
  std::vector<record> links;       ///< Recorded installs.
};

/// @brief The Glaze-facing mirror of link_manifest.
struct manifest {
  std::int64_t       version = 0; ///< 0 when absent; normalised to 1 on the way out.
  std::vector<entry> entries;     ///< Recorded sources.
};

} // namespace wire

auto kind_name(kind value) -> std::string_view {
  return value == kind::skill ? "skill" : "agent";
}

auto kind_dir(kind value) -> std::string_view {
  return value == kind::skill ? "skills" : "agents";
}

auto mode_name(mode value) -> std::string_view {
  return value == mode::symlink ? "symlink" : "copy";
}

auto parse_error_name(parse_error value) -> std::string_view {
  switch (value) {
  case parse_error::no_frontmatter:
    return "NoFrontmatter";
  case parse_error::malformed_frontmatter:
    return "MalformedFrontmatter";
  case parse_error::invalid_vendor:
    return "InvalidVendor";
  case parse_error::kind_mismatch:
    return "KindMismatch";
  case parse_error::invalid_input:
    return "InvalidInput";
  case parse_error::file_not_found:
    return "FileNotFound";
  }
  return "InvalidInput";
}

auto parse_error_reason(parse_error value) -> std::string_view {
  switch (value) {
  case parse_error::no_frontmatter:
    return "no-frontmatter";
  case parse_error::kind_mismatch:
    return "kind-mismatch";
  case parse_error::invalid_vendor:
    return "invalid-vendor";
  case parse_error::malformed_frontmatter:
  case parse_error::invalid_input:
  case parse_error::file_not_found:
    return "invalid-frontmatter";
  }
  return "invalid-frontmatter";
}

auto resolved_vendors(const frontmatter& value) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (value.vendors.empty()) {
    for (const auto vendor : all_vendors) {
      out.emplace_back(vendor);
    }
  } else {
    out = value.vendors;
  }
  std::ranges::sort(out);
  return out;
}

auto lint(const frontmatter& value, std::string_view name) -> std::vector<lint_issue> {
  std::vector<lint_issue> out;
  if (value.description.empty()) {
    out.push_back({lint_severity::warning, "description", "description is empty; vendors surface this as the skill summary"});
  }
  if (value.shadow) {
    out.push_back({lint_severity::warning, "shadow",
                   std::format("shadow:true — install will land as \"{}.md\" (no local- prefix) and may "
                               "replace a canonical install of the same name",
                               name)});
  }
  return out;
}

auto split_frontmatter(std::string_view content) -> std::expected<std::pair<frontmatter, std::string>, parse_error> {
  if (!content.starts_with(k_open_fence)) {
    return std::unexpected(parse_error::no_frontmatter);
  }
  const auto rest = content.substr(k_open_fence.size());

  auto end = rest.find(k_close_fence);
  if (end == std::string_view::npos && rest.ends_with(k_eof_fence)) {
    // A block closed by `\n---` at EOF with no trailing newline. Zig computes
    // `rest.len - 4`, i.e. the offset of that `\n`, and then still skips
    // `end + 5` bytes — one past the end — which is why the body comes out
    // empty rather than out of range.
    end = rest.size() - k_eof_fence.size();
  }
  if (end == std::string_view::npos) {
    return std::unexpected(parse_error::malformed_frontmatter);
  }

  const auto yaml_block  = rest.substr(0, end);
  const auto after_close = end + k_close_fence.size();
  const auto body_src    = after_close < rest.size() ? rest.substr(after_close) : std::string_view{};
  const auto body        = body_src.starts_with('\n') ? body_src.substr(1) : body_src;

  return std::pair{parse_frontmatter_block(yaml_block), std::string{body}};
}

auto parse_file(const std::filesystem::path& path, kind file_kind) -> std::expected<sandbox_file, parse_error> {
  const auto raw = read_file(path);
  if (!raw) {
    return std::unexpected(parse_error::file_not_found);
  }
  auto split = split_frontmatter(*raw);
  if (!split) {
    return std::unexpected(split.error());
  }
  auto& [fm, body] = *split;

  for (const auto& vendor : fm.vendors) {
    if (!is_known_vendor(vendor)) {
      return std::unexpected(parse_error::invalid_vendor);
    }
  }
  if (!fm.kind.empty() && fm.kind != kind_name(file_kind)) {
    return std::unexpected(parse_error::kind_mismatch);
  }

  const auto  source_path = to_absolute(path);
  const auto  base        = source_path.filename().string();
  std::string name;
  if (base == "SKILL.md") {
    if (!source_path.has_parent_path()) {
      return std::unexpected(parse_error::invalid_input);
    }
    name = source_path.parent_path().filename().string();
  } else {
    if (!base.ends_with(".md")) {
      return std::unexpected(parse_error::invalid_input);
    }
    name = base.substr(0, base.size() - 3);
  }

  return sandbox_file{source_path.string(), std::move(name), file_kind, std::move(fm), std::move(body)};
}

namespace {

/// @brief Entries of `dir`, or empty when it cannot be opened.
///
/// Order is the filesystem's, NOT sorted — see walk_sandbox()'s ordering note.
auto iterate_dir(const std::filesystem::path& dir) -> std::vector<std::filesystem::directory_entry> {
  std::error_code                               ec;
  std::vector<std::filesystem::directory_entry> out;
  std::filesystem::directory_iterator           it(dir, ec);
  if (ec) {
    return out;
  }
  for (const auto& entry : it) {
    out.push_back(entry);
  }
  return out;
}

auto is_hidden(const std::string& name) -> bool {
  return name.empty() || name.front() == '.';
}

auto walk_skills_dir(const std::filesystem::path& dir, std::vector<sandbox_file>& files, std::vector<walk_error>& errors)
    -> void {
  for (const auto& entry : iterate_dir(dir)) {
    const auto name = entry.path().filename().string();
    if (is_hidden(name)) {
      continue;
    }
    std::error_code ec;
    if (!entry.is_directory(ec)) {
      if (name.ends_with(".md")) {
        errors.push_back({(dir / name).string(), std::format("legacy flat skill file; run `planar local migrate` to convert to "
                                                             "{}/SKILL.md",
                                                             name.substr(0, name.size() - 3))});
      }
      continue;
    }
    const auto skill_md = dir / name / "SKILL.md";
    if (!path_exists(skill_md)) {
      errors.push_back({skill_md.string(), "skill directory missing SKILL.md"});
      continue;
    }
    auto parsed = parse_file(skill_md, kind::skill);
    if (!parsed) {
      errors.push_back({skill_md.string(), std::string{parse_error_name(parsed.error())}});
      continue;
    }
    files.push_back(std::move(*parsed));
  }
}

auto walk_agents_dir(const std::filesystem::path& dir, std::vector<sandbox_file>& files, std::vector<walk_error>& errors)
    -> void {
  for (const auto& entry : iterate_dir(dir)) {
    const auto      name = entry.path().filename().string();
    std::error_code ec;
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (is_hidden(name) || !name.ends_with(".md")) {
      continue;
    }
    const auto path   = dir / name;
    auto       parsed = parse_file(path, kind::agent);
    if (!parsed) {
      errors.push_back({path.string(), std::string{parse_error_name(parsed.error())}});
      continue;
    }
    files.push_back(std::move(*parsed));
  }
}

} // namespace

auto walk_sandbox(const std::filesystem::path& root) -> walk_result {
  walk_result out;
  if (root.empty()) {
    return out;
  }
  walk_skills_dir(root / "skills", out.files, out.walk_errors);
  walk_agents_dir(root / "agents", out.files, out.walk_errors);

  std::ranges::sort(out.files, [](const sandbox_file& lhs, const sandbox_file& rhs) {
    if (lhs.kind != rhs.kind) {
      return lhs.kind < rhs.kind;
    }
    return lhs.name < rhs.name;
  });
  return out;
}

auto migrate(const std::filesystem::path& sandbox_root, bool dry_run) -> migrate_result {
  migrate_result out;
  if (sandbox_root.empty()) {
    return out;
  }
  const auto skills_dir = sandbox_root / "skills";

  for (const auto& entry : iterate_dir(skills_dir)) {
    const auto      name = entry.path().filename().string();
    std::error_code ec;
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (is_hidden(name) || !name.ends_with(".md")) {
      continue;
    }
    const auto stem     = name.substr(0, name.size() - 3);
    const auto old_path = skills_dir / name;
    const auto new_dir  = skills_dir / stem;
    const auto new_path = new_dir / "SKILL.md";

    migrate_record rec{stem, old_path.string(), new_path.string(), ""};

    if (path_exists(new_dir)) {
      rec.reason = path_exists(new_path) ? std::format("already migrated: {} exists", new_path.string())
                                         : std::format("collision: {} exists but is not a matching skill dir", new_dir.string());
      out.skipped.push_back(std::move(rec));
      continue;
    }

    if (!dry_run) {
      std::error_code mkec;
      std::filesystem::create_directories(new_dir, mkec);
      std::error_code mvec;
      std::filesystem::rename(old_path, new_path, mvec);
    }
    out.migrated.push_back(std::move(rec));
  }

  const auto by_name = [](const migrate_record& lhs, const migrate_record& rhs) { return lhs.name < rhs.name; };
  std::ranges::sort(out.migrated, by_name);
  std::ranges::sort(out.skipped, by_name);
  return out;
}

auto manifest_path(const std::filesystem::path& home_dir, kind manifest_kind) -> std::filesystem::path {
  return home_dir / ".planar" / "local" / std::string{kind_dir(manifest_kind)} / std::string{manifest_filename};
}

auto parse_manifest(std::string_view content) -> std::optional<link_manifest> {
  wire::manifest parsed{};
  // error_on_unknown_keys=false mirrors zig's `.ignore_unknown_fields = true`:
  // a manifest written by a newer Planar must not become unreadable to an
  // older one, because "unreadable" means "every install it records is
  // orphaned".
  constexpr glz::opts opts{.error_on_unknown_keys = false};
  const auto          err = glz::read<opts>(parsed, content);
  if (err) {
    return std::nullopt;
  }

  link_manifest out;
  // Zig normalises a 0 (or absent) version to 1 AFTER parsing, rather than
  // rejecting it. Preserved: a hand-edited manifest missing `version` still
  // loads.
  out.version = parsed.version == 0 ? 1 : parsed.version;
  for (auto& entry : parsed.entries) {
    manifest_entry converted{std::move(entry.name), std::move(entry.source_path), {}};
    for (auto& link : entry.links) {
      converted.links.push_back({std::move(link.vendor), std::move(link.target_path), std::move(link.source_path),
                                 link.mode == "copy" ? mode::copy : mode::symlink, std::move(link.linked_at)});
    }
    out.entries.push_back(std::move(converted));
  }
  return out;
}

auto serialize_manifest(const link_manifest& value) -> std::string {
  // Hand-rolled rather than delegated, because the target is not "valid JSON"
  // but the exact bytes zig's `std.json.Stringify` with `.whitespace =
  // .indent_2` produces: two-space indent, `": "` after every key, no space
  // after a comma, `[]` rendered inline when empty, and one trailing newline
  // after the closing brace. Oracle-captured from a real
  // `.link-manifest.json`.
  std::string out;
  out.append("{\n");
  out.append(std::format("  \"version\": {},\n", value.version));
  out.append("  \"entries\": ");
  if (value.entries.empty()) {
    out.append("[]\n}\n");
    return out;
  }
  out.append("[\n");
  for (std::size_t i = 0; i < value.entries.size(); ++i) {
    const auto& entry = value.entries[i];
    out.append("    {\n      \"name\": ");
    json_text::append_json_string(out, entry.name);
    out.append(",\n      \"source_path\": ");
    json_text::append_json_string(out, entry.source_path);
    out.append(",\n      \"links\": ");
    if (entry.links.empty()) {
      out.append("[]\n");
    } else {
      out.append("[\n");
      for (std::size_t j = 0; j < entry.links.size(); ++j) {
        const auto& link = entry.links[j];
        out.append("        {\n          \"vendor\": ");
        json_text::append_json_string(out, link.vendor);
        out.append(",\n          \"target_path\": ");
        json_text::append_json_string(out, link.target_path);
        out.append(",\n          \"source_path\": ");
        json_text::append_json_string(out, link.source_path);
        out.append(",\n          \"mode\": ");
        json_text::append_json_string(out, mode_name(link.mode));
        out.append(",\n          \"linked_at\": ");
        json_text::append_json_string(out, link.linked_at);
        out.append("\n        }");
        out.append(j + 1 == entry.links.size() ? "\n" : ",\n");
      }
      out.append("      ]\n");
    }
    out.append("    }");
    out.append(i + 1 == value.entries.size() ? "\n" : ",\n");
  }
  out.append("  ]\n}\n");
  return out;
}

auto load_manifest(const std::filesystem::path& home_dir, kind manifest_kind) -> std::optional<link_manifest> {
  const auto path = manifest_path(home_dir, manifest_kind);
  const auto raw  = read_file(path);
  if (!raw) {
    // Absent is the FIRST-RUN state, not a failure — every caller relies on
    // this, so it must not be conflated with an unparseable file.
    return link_manifest{1, {}};
  }
  return parse_manifest(*raw);
}

auto save_manifest(const std::filesystem::path& home_dir, kind manifest_kind, const link_manifest& value) -> bool {
  const auto path = manifest_path(home_dir, manifest_kind);
  const auto body = serialize_manifest(value);
  const auto tmp  = std::filesystem::path{path.string() + ".tmp"};
  if (!write_file(tmp, body)) {
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

auto resolve_home_and_root(const std::function<std::optional<std::string>(std::string_view)>& env_lookup)
    -> std::optional<home_and_root> {
  auto home = env_lookup("PLANAR_LOCAL_HOME");
  if (!home || home->empty()) {
    home = env_lookup("HOME");
  }
  if (!home || home->empty()) {
    return std::nullopt;
  }
  const std::filesystem::path home_dir{*home};
  return home_and_root{home_dir, home_dir / ".planar" / "local"};
}

auto lookup_kind_for_name(const std::filesystem::path& sandbox_root, std::string_view name) -> std::optional<kind> {
  if (path_exists(sandbox_root / "skills" / std::string{name} / "SKILL.md")) {
    return kind::skill;
  }
  if (path_exists(sandbox_root / "agents" / std::format("{}.md", name))) {
    return kind::agent;
  }
  return std::nullopt;
}

auto parse_kind(std::optional<std::string_view> raw) -> std::optional<kind> {
  const auto value = raw.value_or("skill");
  if (value == "skill" || value == "skills") {
    return kind::skill;
  }
  if (value == "agent" || value == "agents") {
    return kind::agent;
  }
  return std::nullopt;
}

} // namespace planar::engine::local::manifest
