/// @file link.cpp
/// @brief Implementation of `planar.engine.local.link` (plan 996 task 6109,
/// retargeted by plan 1104 task 7220). See link.cppm for the projection table,
/// the presence and ownership rules and the liveness states.

module planar.engine.local.link;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::link {

namespace fs = std::filesystem;

namespace {

auto path_exists(const fs::path& path) -> bool {
  std::error_code ec;
  if (fs::exists(path, ec)) {
    return true;
  }
  // A DANGLING symlink does not "exist" but is very much there.
  const auto st = fs::symlink_status(path, ec);
  return !ec && st.type() != fs::file_type::not_found;
}

/// @brief The symlink's target, or nullopt when `path` is not a symlink.
auto read_link(const fs::path& path) -> std::optional<fs::path> {
  std::error_code ec;
  auto            target = fs::read_symlink(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return target;
}

auto remove_path_if_exists(const fs::path& path) -> void {
  std::error_code ec;
  // remove() unlinks a symlink without following it, which is what we want:
  // deleting through a directory link must never delete what it points at.
  if (fs::remove(path, ec)) {
    return;
  }
  // remove() refuses a non-empty directory; fall back to a recursive delete of it.
  fs::remove_all(path, ec);
}

auto read_all(const fs::path& path) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  return buffer.str();
}

auto write_all(const fs::path& path, std::string_view data) -> bool {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.flush();
  return static_cast<bool>(out);
}

// --- text forms -------------------------------------------------------------

constexpr std::string_view k_ws = " \t\r\n\v\f";

auto strip(std::string_view value) -> std::string_view {
  const auto first = value.find_first_not_of(k_ws);
  if (first == std::string_view::npos) {
    return {};
  }
  return value.substr(first, value.find_last_not_of(k_ws) - first + 1);
}

auto without_cr(std::string_view line) -> std::string_view {
  while (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  return line;
}

/// @brief A source split at its frontmatter fence.
struct front {
  std::vector<std::string_view> lines;           ///< The frontmatter lines, without their `\n`.
  std::size_t                   header_end  = 0; ///< Offset just past the opening `---\n`.
  std::size_t                   close_begin = 0; ///< Offset of the closing `---` line.
  std::string_view              after;           ///< Everything after the closing line's `\n`.
};

/// @brief Split `content` at a leading `---` line and the next `---` line.
/// @param content The bytes.
/// @param tolerate_cr Compare fence lines with trailing `\r` removed (the Python script does).
/// @return The split, or nullopt when the block is absent or unclosed.
auto split_front(std::string_view content, bool tolerate_cr) -> std::optional<front> {
  const auto matches  = [&](std::string_view line) { return (tolerate_cr ? without_cr(line) : line) == "---"; };
  const auto first_nl = content.find('\n');
  if (first_nl == std::string_view::npos || !matches(content.substr(0, first_nl))) {
    return std::nullopt;
  }
  front       out;
  std::size_t pos = first_nl + 1;
  out.header_end  = pos;
  while (pos <= content.size()) {
    const auto nl   = content.find('\n', pos);
    const auto end  = nl == std::string_view::npos ? content.size() : nl;
    const auto line = content.substr(pos, end - pos);
    if (matches(line)) {
      out.close_begin = pos;
      out.after       = nl == std::string_view::npos ? std::string_view{} : content.substr(nl + 1);
      return out;
    }
    if (nl == std::string_view::npos) {
      return std::nullopt;
    }
    out.lines.push_back(line);
    pos = nl + 1;
  }
  return std::nullopt;
}

auto unquote(std::string_view value) -> std::string_view {
  value = strip(value);
  if (value.size() >= 2 && value.front() == value.back() && (value.front() == '"' || value.front() == '\'')) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

/// @brief The last top-level `key:` value of a frontmatter block, as the Python renderer reads it.
auto top_level_value(std::span<const std::string_view> lines, std::string_view key) -> std::string {
  std::string result;
  for (auto raw : lines) {
    const auto line = without_cr(raw);
    if (strip(line).empty() || line.substr(line.find_first_not_of(k_ws)).starts_with('#')) {
      continue;
    }
    if (line.front() == ' ' || line.front() == '\t') {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    if (strip(line.substr(0, colon)) == key) {
      result = std::string{unquote(line.substr(colon + 1))};
    }
  }
  return result;
}

auto toml_string(std::string_view text) -> std::string {
  std::string out = "\"";
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (ch < 0x20 || ch == 0x7F) {
        out += std::format("\\u{:04X}", static_cast<unsigned>(ch));
      } else {
        out.push_back(raw);
      }
    }
  }
  out.push_back('"');
  return out;
}

/// @brief Whether `install.sh`'s opencode_derive double-quotes a description.
auto opencode_needs_quotes(std::string_view desc) -> bool {
  if (desc.starts_with('"') || desc.starts_with('\'')) {
    return false;
  }
  if (desc.contains(": ") || desc.contains(" #") || desc.ends_with(':')) {
    return true;
  }
  return !desc.empty() && std::string_view{"-?:,[]{}#&*!|>%@`"}.contains(desc.front());
}

// --- trees ------------------------------------------------------------------

/// @brief One file or symlink of a projection.
struct node {
  std::string rel;                     ///< Path relative to the projection root; empty for a single file.
  bool        is_link = false;         ///< A symlink; `data` is its target.
  std::string data;                    ///< File bytes, or the link target.
  fs::perms   perms = fs::perms::none; ///< Source permissions, applied on write; never compared.
};

using tree = std::vector<node>;

auto trees_equal(const tree& lhs, const tree& rhs) -> bool {
  return std::ranges::equal(
      lhs, rhs, [](const node& a, const node& b) { return a.rel == b.rel && a.is_link == b.is_link && a.data == b.data; });
}

auto read_tree(const fs::path& root) -> std::optional<tree> {
  tree                             out;
  std::error_code                  ec;
  fs::recursive_directory_iterator it(root, ec);
  if (ec) {
    return std::nullopt;
  }
  for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      return std::nullopt;
    }
    std::error_code sec;
    const auto      st  = it->symlink_status(sec);
    const auto      rel = fs::relative(it->path(), root, sec).generic_string();
    if (sec) {
      return std::nullopt;
    }
    if (fs::is_symlink(st)) {
      const auto target = read_link(it->path());
      out.push_back({rel, true, target ? target->string() : std::string{}, fs::perms::none});
    } else if (fs::is_regular_file(st)) {
      auto data = read_all(it->path());
      if (!data) {
        return std::nullopt;
      }
      out.push_back({rel, false, std::move(*data), st.permissions()});
    }
  }
  std::ranges::sort(out, {}, &node::rel);
  return out;
}

auto layout_for_record(manifest::kind row_kind, std::string_view vendor) -> std::optional<layout> {
  if (row_kind == manifest::kind::skill) {
    if (vendor == "claude" || vendor == "shared" || vendor == "antigravity") {
      return layout::skill_dir;
    }
    return std::nullopt;
  }
  if (vendor == "claude" || vendor == "gemini" || vendor == "antigravity") {
    return layout::agent_md;
  }
  if (vendor == "copilot") {
    return layout::agent_copilot;
  }
  if (vendor == "opencode") {
    return layout::agent_opencode;
  }
  if (vendor == "codex") {
    return layout::agent_toml;
  }
  return std::nullopt;
}

/// @brief The fresh projection of a source.
/// @param lay The destination layout.
/// @param source_path The skill's `SKILL.md`, or the agent's `.md`.
/// @param name The source's name (unprefixed).
auto project_for(layout lay, const fs::path& source_path, std::string_view name) -> std::optional<tree> {
  const auto pn = projected_name(name);
  if (lay == layout::skill_dir) {
    auto files = read_tree(source_path.parent_path());
    if (!files) {
      return std::nullopt;
    }
    bool has_skill_md = false;
    for (auto& entry : *files) {
      if (entry.rel == "SKILL.md" && !entry.is_link) {
        entry.data   = rewrite_name(entry.data, pn);
        has_skill_md = true;
      }
    }
    if (!has_skill_md) {
      return std::nullopt;
    }
    return files;
  }
  const auto content = read_all(source_path);
  if (!content) {
    return std::nullopt;
  }
  std::optional<std::string> data;
  switch (lay) {
  case layout::agent_md:
  case layout::agent_copilot:
    data = rewrite_name(*content, pn);
    break;
  case layout::agent_opencode:
    data = opencode_form(*content);
    break;
  case layout::agent_toml:
    data = codex_toml(pn, *content);
    break;
  case layout::skill_dir:
    break;
  }
  if (!data) {
    return std::nullopt;
  }
  std::error_code ec;
  const auto      perms = fs::status(source_path, ec).permissions();
  return tree{{"", false, std::move(*data), perms}};
}

enum class state { absent, equal, differs };

auto read_installed(const fs::path& path, layout lay) -> tree {
  std::error_code ec;
  const auto      st = fs::symlink_status(path, ec);
  if (fs::is_symlink(st)) {
    const auto target = read_link(path);
    return {{"", true, target ? target->string() : std::string{}, fs::perms::none}};
  }
  if (lay == layout::skill_dir) {
    if (fs::is_directory(st)) {
      if (auto got = read_tree(path)) {
        return std::move(*got);
      }
    }
    return {{"", true, "<not a directory>", fs::perms::none}};
  }
  if (fs::is_regular_file(st)) {
    if (auto data = read_all(path)) {
      return {{"", false, std::move(*data), fs::perms::none}};
    }
  }
  return {{"", true, "<not a file>", fs::perms::none}};
}

auto state_of(const fs::path& path, layout lay, const tree& expected) -> state {
  std::error_code ec;
  const auto      st = fs::symlink_status(path, ec);
  if (ec || st.type() == fs::file_type::not_found) {
    return state::absent;
  }
  return trees_equal(read_installed(path, lay), expected) ? state::equal : state::differs;
}

/// @brief Write a projection atomically: a sibling temp path, then a rename.
auto install(const fs::path& target, layout lay, const tree& content) -> bool {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  const fs::path tmp = target.parent_path() / std::format(".{}.planar-tmp", target.filename().string());
  remove_path_if_exists(tmp);

  const auto set_perms = [](const fs::path& path, fs::perms perms) {
    if (perms != fs::perms::none) {
      std::error_code pec;
      fs::permissions(path, perms, pec);
    }
  };

  if (lay == layout::skill_dir) {
    fs::create_directories(tmp, ec);
    for (const auto& entry : content) {
      const auto dest = tmp / entry.rel;
      fs::create_directories(dest.parent_path(), ec);
      if (entry.is_link) {
        fs::create_symlink(entry.data, dest, ec);
        if (ec) {
          remove_path_if_exists(tmp);
          return false;
        }
      } else {
        if (!write_all(dest, entry.data)) {
          remove_path_if_exists(tmp);
          return false;
        }
        set_perms(dest, entry.perms);
      }
    }
  } else {
    if (content.size() != 1 || !write_all(tmp, content.front().data)) {
      remove_path_if_exists(tmp);
      return false;
    }
    set_perms(tmp, content.front().perms);
  }

  fs::rename(tmp, target, ec);
  if (!ec) {
    return true;
  }
  // Rename onto a non-empty directory (or across kinds) fails; clear the way.
  remove_path_if_exists(target);
  ec.clear();
  fs::rename(tmp, target, ec);
  if (ec) {
    remove_path_if_exists(tmp);
    return false;
  }
  return true;
}

// --- ownership --------------------------------------------------------------

auto sandbox_root(const fs::path& home_dir) -> fs::path {
  return home_dir / ".planar" / "local";
}

/// @brief Whether `path` is a symlink pointing into the sandbox.
auto links_into_sandbox(const fs::path& path, const fs::path& home_dir) -> bool {
  const auto target = read_link(path);
  if (!target) {
    return false;
  }
  const auto root = sandbox_root(home_dir).string();
  const auto text = target->string();
  return text == root || text.starts_with(root + "/");
}

/// @brief Every target path recorded in either manifest.
auto recorded_targets(const fs::path& home_dir) -> std::set<std::string> {
  std::set<std::string> out;
  for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
    if (const auto loaded = manifest::load_manifest(home_dir, row_kind)) {
      for (const auto& entry : loaded->entries) {
        for (const auto& row : entry.links) {
          out.insert(row.target_path);
        }
      }
    }
  }
  return out;
}

auto is_owned(const fs::path& path, const fs::path& home_dir, const std::set<std::string>& recorded) -> bool {
  return recorded.contains(path.string()) || links_into_sandbox(path, home_dir);
}

/// @brief The path to name when `target` is foreign: the first differing file inside a skill copy, else the target.
auto conflict_path(const fs::path& target, layout lay, const tree& expected) -> std::string {
  if (lay == layout::skill_dir) {
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(target, ec))) {
      for (const auto& entry : expected) {
        const auto path = target / entry.rel;
        const auto cur  = read_installed(path, layout::agent_md);
        const bool same = cur.size() == 1 && cur.front().is_link == entry.is_link && cur.front().data == entry.data;
        if (!same && path_exists(path)) {
          return path.string();
        }
      }
    }
  }
  return target.string();
}

auto is_legacy_path(const fs::path& path) -> bool {
  const auto parent = path.parent_path().generic_string();
  return parent.ends_with("/.claude/commands") || parent.ends_with("/.codex/skills") || parent.ends_with("/.copilot/skills") ||
         parent.ends_with("/.planar/agents");
}

/// @brief Whether an OLD projection may be removed: a symlink into the sandbox, or a copy equal to its recorded source.
auto legacy_owned(const fs::path& path, const fs::path& source, const fs::path& home_dir) -> bool {
  std::error_code ec;
  const auto      st = fs::symlink_status(path, ec);
  if (fs::is_symlink(st)) {
    return links_into_sandbox(path, home_dir);
  }
  if (fs::is_regular_file(st)) {
    const auto mine   = read_all(path);
    const auto theirs = read_all(source);
    return mine && theirs && *mine == *theirs;
  }
  if (fs::is_directory(st)) {
    const auto src_dir = fs::is_directory(source, ec) ? source : source.parent_path();
    const auto mine    = read_tree(path);
    const auto theirs  = read_tree(src_dir);
    return mine && theirs && trees_equal(*mine, *theirs);
  }
  return false;
}

/// @brief Whether `path` still carries the name Planar wrote into its own projection.
auto carries_planar_name(const fs::path& path, layout lay, std::string_view projected) -> bool {
  for (const auto& entry : read_installed(path, lay)) {
    if (entry.is_link || (lay == layout::skill_dir && entry.rel != "SKILL.md")) {
      continue;
    }
    switch (lay) {
    case layout::skill_dir:
    case layout::agent_md:
    case layout::agent_copilot:
      if (const auto split = split_front(entry.data, false); split && top_level_value(split->lines, "name") == projected) {
        return true;
      }
      break;
    case layout::agent_toml:
      if (entry.data.starts_with(std::format("name = {}\n", toml_string(projected)))) {
        return true;
      }
      break;
    case layout::agent_opencode:
      if (entry.data.starts_with("---\ndescription:") && entry.data.contains("\nmode: subagent\n")) {
        return true;
      }
      break;
    }
  }
  return false;
}

/// @brief Whether a RECORDED projection may be deleted.
///
/// A path that is gone is trivially removable. Otherwise it must still be a
/// Planar-made projection: a symlink into the sandbox, a copy equal to its
/// recorded source (old or fresh projection), or a copy still carrying the
/// projected name when the source has changed or vanished. Anything else was
/// replaced by the operator and is left alone.
auto removal_owned(manifest::kind row_kind, std::string_view name, const manifest::manifest_record& row, const fs::path& home_dir)
    -> bool {
  const fs::path path{row.target_path};
  if (!path_exists(path) || links_into_sandbox(path, home_dir) || legacy_owned(path, fs::path{row.source_path}, home_dir)) {
    return true;
  }
  const auto lay = layout_for_record(row_kind, row.vendor);
  if (!lay) {
    return false;
  }
  if (path_exists(fs::path{row.source_path})) {
    if (const auto expected = project_for(*lay, fs::path{row.source_path}, name);
        expected && state_of(path, *lay, *expected) == state::equal) {
      return true;
    }
  }
  return carries_planar_name(path, *lay, projected_name(name));
}

// --- manifest plumbing ------------------------------------------------------

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
/// `skipped`, `dry-run` and `refused` records are DROPPED here. That filter is
/// what makes `--vendor` narrow the recorded set rather than merge into it, and
/// what keeps `--dry-run` from recording projections it never made.
auto to_manifest_entry(std::string_view name, std::string_view source_path, std::span<const target_record> records)
    -> manifest::manifest_entry {
  manifest::manifest_entry entry{std::string{name}, std::string{source_path}, {}};
  for (const auto& rec : records) {
    if (rec.action == "skipped" || rec.action == "dry-run" || rec.action == "refused") {
      continue;
    }
    entry.links.push_back({rec.vendor, rec.target_path, rec.source_path, rec.mode.value_or(manifest::mode::copy), rec.linked_at});
  }
  return entry;
}

auto put_manifest_entry(manifest::link_manifest& value, manifest::manifest_entry entry) -> void {
  if (const auto idx = find_manifest_entry(value, entry.name)) {
    value.entries[*idx] = std::move(entry);
  } else {
    value.entries.push_back(std::move(entry));
  }
  value.version = 1;
}

/// @brief Save `next` unless it would write the bytes already on disk.
auto save_if_changed(const fs::path& home_dir, manifest::kind row_kind, const manifest::link_manifest& before,
                     const manifest::link_manifest& next) -> void {
  if (manifest::serialize_manifest(before) != manifest::serialize_manifest(next)) {
    manifest::save_manifest(home_dir, row_kind, next);
  }
}

auto agent_vendors_present(const target_query& query) -> std::set<std::string> {
  const auto&           home = query.home_dir;
  std::set<std::string> out;
  std::error_code       ec;
  if (fs::is_directory(home / ".claude", ec)) {
    out.insert("claude");
  }
  if (query.codex_home.has_value() || fs::is_directory(home / ".codex", ec)) {
    out.insert("codex");
  }
  if (fs::is_directory(home / ".copilot", ec)) {
    out.insert("copilot");
  }
  if (fs::exists(home / ".gemini" / "settings.json", ec)) {
    out.insert("gemini");
  }
  if (fs::is_directory(home / ".gemini" / "antigravity-cli", ec)) {
    out.insert("antigravity");
  }
  if (fs::is_directory(home / ".config" / "opencode", ec)) {
    out.insert("opencode");
  }
  return out;
}

} // namespace

auto projected_name(std::string_view name) -> std::string {
  return std::format("{}{}", projection_prefix, name);
}

auto rewrite_name(std::string_view content, std::string_view name) -> std::string {
  const auto split = split_front(content, false);
  if (!split) {
    return std::string{content};
  }
  const auto  replacement = std::format("name: {}\n", name);
  std::string out{content.substr(0, split->header_end)};
  bool        replaced = false;
  std::size_t pos      = split->header_end;
  while (pos < split->close_begin) {
    const auto nl   = content.find('\n', pos);
    const auto line = content.substr(pos, nl + 1 - pos);
    if (line.starts_with("name:")) {
      // Keep the line's own ending: a CRLF source stays CRLF.
      out += line.ends_with("\r\n") ? std::format("name: {}\r\n", name) : replacement;
      replaced = true;
    } else {
      out += line;
    }
    pos = nl + 1;
  }
  if (!replaced) {
    out.insert(split->header_end, replacement);
  }
  out += content.substr(split->close_begin);
  return out;
}

auto opencode_form(std::string_view content) -> std::optional<std::string> {
  const auto split = split_front(content, false);
  if (!split) {
    return std::nullopt;
  }
  std::string desc;
  for (const auto line : split->lines) {
    if (line.starts_with("description:")) {
      auto value = line.substr(std::string_view{"description:"}.size());
      value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
      desc = std::string{value};
    }
  }
  if (desc.empty()) {
    return std::nullopt;
  }
  if (opencode_needs_quotes(desc)) {
    std::string escaped;
    for (const char ch : desc) {
      if (ch == '\\' || ch == '"') {
        escaped.push_back('\\');
      }
      escaped.push_back(ch);
    }
    desc = std::format("\"{}\"", escaped);
  }
  std::string out = std::format("---\ndescription: {}\nmode: subagent\n---\n", desc);
  out += split->after;
  if (!split->after.empty() && !split->after.ends_with('\n')) {
    out.push_back('\n');
  }
  return out;
}

auto codex_toml(std::string_view name, std::string_view content) -> std::optional<std::string> {
  const auto split = split_front(content, true);
  if (!split) {
    return std::nullopt;
  }
  const auto desc = top_level_value(split->lines, "description");
  if (desc.empty() || name.empty()) {
    return std::nullopt;
  }
  auto body = split->after;
  if (body.starts_with("\r\n")) {
    body.remove_prefix(2);
  } else if (body.starts_with('\n')) {
    body.remove_prefix(1);
  }
  std::string out;
  out += std::format("name = {}\n", toml_string(name));
  out += std::format("description = {}\n", toml_string(desc));
  out += std::format("developer_instructions = {}\n", toml_string(body));
  return out;
}

auto vendor_matches(std::string_view record_vendor, std::string_view filter) -> bool {
  if (record_vendor == filter) {
    return true;
  }
  return record_vendor == "shared" && (filter == "codex" || filter == "copilot" || filter == "gemini" || filter == "opencode");
}

auto targets(const target_query& query) -> std::vector<target> {
  std::vector<target> out;
  if (query.home_dir.empty() || query.name.empty()) {
    return out;
  }
  const auto& home    = query.home_dir;
  const auto  pn      = projected_name(query.name);
  const auto  present = agent_vendors_present(query);
  const auto  has     = [&](const std::string& vendor) { return present.contains(vendor); };

  if (query.source_kind == manifest::kind::agent) {
    const auto codex_root = query.codex_home.value_or(home / ".codex");
    if (has("claude")) {
      out.push_back({"claude", layout::agent_md, (home / ".claude" / "agents" / (pn + ".md")).string(), query.source_path});
    }
    if (has("codex")) {
      out.push_back({"codex", layout::agent_toml, (codex_root / "agents" / (pn + ".toml")).string(), query.source_path});
    }
    if (has("copilot")) {
      out.push_back(
          {"copilot", layout::agent_copilot, (home / ".copilot" / "agents" / (pn + ".agent.md")).string(), query.source_path});
    }
    if (has("gemini")) {
      out.push_back({"gemini", layout::agent_md, (home / ".gemini" / "agents" / (pn + ".md")).string(), query.source_path});
    }
    if (has("antigravity")) {
      out.push_back({"antigravity", layout::agent_md, (home / ".gemini" / "antigravity-cli" / "agents" / (pn + ".md")).string(),
                     query.source_path});
    }
    if (has("opencode")) {
      out.push_back({"opencode", layout::agent_opencode, (home / ".config" / "opencode" / "agents" / (pn + ".md")).string(),
                     query.source_path});
    }
  } else {
    const auto wants = [&](std::string_view vendor) { return std::ranges::find(query.vendors, vendor) != query.vendors.end(); };
    const bool shared_vendors = wants("codex") || wants("copilot") || wants("gemini") || wants("opencode");
    if (has("claude") && wants("claude")) {
      out.push_back({"claude", layout::skill_dir, (home / ".claude" / "skills" / pn).string(), query.source_path});
    }
    if (shared_vendors && (has("codex") || has("copilot") || has("gemini") || has("opencode"))) {
      out.push_back({"shared", layout::skill_dir, (home / ".agents" / "skills" / pn).string(), query.source_path});
    }
    if (shared_vendors && has("antigravity")) {
      out.push_back(
          {"antigravity", layout::skill_dir, (home / ".gemini" / "antigravity-cli" / "skills" / pn).string(), query.source_path});
    }
  }
  std::ranges::sort(out, [](const target& lhs, const target& rhs) { return lhs.vendor < rhs.vendor; });
  return out;
}

namespace {

auto query_for(const manifest::sandbox_file& file, const fs::path& home_dir, const std::optional<fs::path>& codex_home)
    -> target_query {
  return {home_dir, codex_home, file.source_path, file.name, file.kind, manifest::resolved_vendors(file.frontmatter)};
}

} // namespace

auto find_conflict(std::span<const manifest::sandbox_file> files, const link_options& options) -> std::optional<std::string> {
  if (options.home_dir.empty()) {
    return std::nullopt;
  }
  const auto recorded = recorded_targets(options.home_dir);
  for (const auto& file : files) {
    for (const auto& tgt : targets(query_for(file, options.home_dir, options.codex_home))) {
      if (options.vendor_filter && !vendor_matches(tgt.vendor, *options.vendor_filter)) {
        continue;
      }
      const auto expected = project_for(tgt.layout, file.source_path, file.name);
      if (!expected) {
        continue;
      }
      const fs::path path{tgt.target_path};
      if (state_of(path, tgt.layout, *expected) == state::differs && !is_owned(path, options.home_dir, recorded)) {
        return conflict_path(path, tgt.layout, *expected);
      }
    }
  }
  return std::nullopt;
}

auto link(const manifest::sandbox_file& file, const link_options& options) -> link_result {
  link_result result{file.name, file.kind, {}};
  if (options.home_dir.empty()) {
    return result;
  }
  auto       loaded   = manifest::load_manifest(options.home_dir, file.kind);
  const auto recorded = recorded_targets(options.home_dir);

  std::map<std::string, std::string> prior_stamps;
  if (loaded) {
    if (const auto idx = find_manifest_entry(*loaded, file.name)) {
      for (const auto& row : loaded->entries[*idx].links) {
        prior_stamps[row.target_path] = row.linked_at;
      }
    }
  }

  for (const auto& tgt : targets(query_for(file, options.home_dir, options.codex_home))) {
    target_record rec{tgt.vendor, tgt.target_path, file.source_path, std::nullopt, "", "", ""};

    if (options.vendor_filter && !vendor_matches(rec.vendor, *options.vendor_filter)) {
      rec.action = "skipped";
      result.records.push_back(std::move(rec));
      continue;
    }
    const auto expected = project_for(tgt.layout, file.source_path, file.name);
    if (!expected) {
      rec.action  = "skipped";
      rec.warning = "this source has no form for this vendor (an agent needs a description); not projected";
      result.records.push_back(std::move(rec));
      continue;
    }
    rec.mode = manifest::mode::copy;
    if (options.dry_run) {
      rec.action = "dry-run";
      result.records.push_back(std::move(rec));
      continue;
    }

    const fs::path path{tgt.target_path};
    const auto     current = state_of(path, tgt.layout, *expected);
    if (current == state::equal) {
      rec.action          = "unchanged";
      const auto previous = prior_stamps.find(rec.target_path);
      rec.linked_at       = previous != prior_stamps.end() && !previous->second.empty() ? previous->second : options.now;
    } else if (current == state::differs && !is_owned(path, options.home_dir, recorded)) {
      rec.action  = "refused";
      rec.mode    = std::nullopt;
      rec.warning = "exists and is not a Planar projection; left untouched";
    } else if (!install(path, tgt.layout, *expected)) {
      rec.action  = "refused";
      rec.mode    = std::nullopt;
      rec.warning = "could not write the projection";
    } else {
      rec.action    = current == state::differs ? "updated" : "created";
      rec.linked_at = options.now;
    }
    result.records.push_back(std::move(rec));
  }

  if (!options.dry_run && loaded) {
    auto next = *loaded;
    put_manifest_entry(next, to_manifest_entry(file.name, file.source_path, result.records));
    save_if_changed(options.home_dir, file.kind, *loaded, next);
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

  unlink_result result{std::string{name}, unlink_kind, {}, "", {}};

  if (const auto idx = find_manifest_entry(*loaded, name)) {
    for (const auto& row : loaded->entries[*idx].links) {
      if (!removal_owned(unlink_kind, name, row, options.home_dir)) {
        result.skipped.push_back(
            {row.vendor, row.target_path, row.source_path, row.mode, "skipped", "skipped: not owned", row.linked_at});
        continue;
      }
      remove_path_if_exists(fs::path{row.target_path});
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
      fs::remove_all(dir, ec);
      result.purged_file = dir.string();
    } else {
      const auto path = options.home_dir / ".planar" / "local" / "agents" / std::format("{}.md", name);
      fs::remove(path, ec);
      result.purged_file = path.string();
    }
    // The error is DISCARDED: `purged_file` means "the path purge targeted",
    // not "the path that was deleted".
  }
  return result;
}

auto is_legacy(const manifest::manifest_record& row) -> bool {
  return row.mode == manifest::mode::symlink || is_legacy_path(fs::path{row.target_path});
}

auto classify_existing(manifest::kind source_kind, std::string_view name, const manifest::manifest_record& row)
    -> std::string_view {
  if (is_legacy(row)) {
    return "legacy";
  }
  const fs::path target{row.target_path};
  if (!path_exists(target)) {
    return "missing";
  }
  const auto lay = layout_for_record(source_kind, row.vendor);
  if (!lay || !path_exists(fs::path{row.source_path})) {
    return "broken";
  }
  const auto expected = project_for(*lay, fs::path{row.source_path}, name);
  if (!expected) {
    return "broken";
  }
  return state_of(target, *lay, *expected) == state::equal ? "live" : "stale";
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
        out.push_back({entry.name,
                       row_kind,
                       {row.vendor, row.target_path, row.source_path, row.mode,
                        std::string{classify_existing(row_kind, entry.name, row)}, "", row.linked_at}});
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

namespace {

/// @brief One change the reconcile plan will apply.
struct planned_write {
  fs::path       path;    ///< The destination.
  layout         lay;     ///< Its layout.
  tree           content; ///< What to write.
  manifest::kind kind;    ///< Which manifest records it.
};

} // namespace

auto reconcile(const reconcile_options& options) -> std::expected<std::vector<reconcile_action>, reconcile_error> {
  using cause = reconcile_error::cause;
  if (options.home_dir.empty()) {
    return std::unexpected(reconcile_error{cause::manifest_unreadable, {}});
  }
  const auto& home = options.home_dir;

  std::map<manifest::kind, manifest::link_manifest> before;
  for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
    auto loaded = manifest::load_manifest(home, row_kind);
    if (!loaded) {
      return std::unexpected(reconcile_error{cause::manifest_unreadable, {}});
    }
    before.emplace(row_kind, std::move(*loaded));
  }
  const auto recorded = recorded_targets(home);
  const auto walked   = manifest::walk_sandbox(sandbox_root(home));

  std::vector<reconcile_action>                     actions;
  std::vector<fs::path>                             removals;
  std::vector<planned_write>                        writes;
  std::map<manifest::kind, manifest::link_manifest> after;

  for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
    const auto&                                     old_manifest = before.at(row_kind);
    std::map<std::string, manifest::manifest_entry> computed;
    std::vector<std::string>                        computed_order;

    // Entries whose source is gone: remove what they recorded.
    for (const auto& entry : old_manifest.entries) {
      if (path_exists(fs::path{entry.source_path})) {
        continue;
      }
      std::vector<std::string> removed;
      std::vector<std::string> foreign;
      for (const auto& row : entry.links) {
        if (removal_owned(row_kind, entry.name, row, home)) {
          removed.push_back(row.target_path);
          removals.emplace_back(row.target_path);
        } else {
          foreign.push_back(row.target_path);
        }
      }
      actions.push_back({entry.name, row_kind, "source-missing", entry.source_path, std::move(removed)});
      if (!foreign.empty()) {
        actions.push_back({entry.name, row_kind, "not-owned", entry.source_path, std::move(foreign)});
      }
    }

    for (const auto& file : walked.files) {
      if (file.kind != row_kind) {
        continue;
      }
      const manifest::manifest_entry* old_entry = nullptr;
      if (const auto idx = find_manifest_entry(old_manifest, file.name)) {
        old_entry = &old_manifest.entries[*idx];
      }
      std::map<std::string, std::string> prior_stamps;
      std::set<std::string>              legacy_seen;
      std::vector<std::string>           legacy_removed;
      std::vector<std::string>           stale;
      std::vector<std::string>           missing;

      // Old projections, recorded.
      if (old_entry != nullptr) {
        for (const auto& row : old_entry->links) {
          prior_stamps[row.target_path] = row.linked_at;
          if (!is_legacy(row)) {
            continue;
          }
          legacy_seen.insert(row.target_path);
          if (legacy_owned(fs::path{row.target_path}, fs::path{row.source_path}, home)) {
            legacy_removed.push_back(row.target_path);
            removals.emplace_back(row.target_path);
          }
        }
      }
      // Old projections by name, when the manifest never recorded them.
      std::vector<fs::path> candidates;
      if (file.kind == manifest::kind::skill) {
        candidates = {home / ".claude" / "commands" / std::format("local-{}.md", file.name),
                      home / ".codex" / "skills" / std::format("local-{}", file.name),
                      home / ".copilot" / "skills" / std::format("local-{}", file.name)};
      } else {
        candidates = {home / ".planar" / "agents" / std::format("local-{}.md", file.name)};
      }
      for (const auto& candidate : candidates) {
        if (!legacy_seen.contains(candidate.string()) && links_into_sandbox(candidate, home)) {
          legacy_removed.push_back(candidate.string());
          removals.push_back(candidate);
        }
      }

      // A source Planar has never projected, or that was unlinked on purpose, is
      // not resurrected: reconcile acts on sources it already tracks or is
      // migrating from an old projection.
      if (old_entry == nullptr && legacy_seen.empty() && legacy_removed.empty()) {
        continue;
      }

      manifest::manifest_entry entry{file.name, file.source_path, {}};
      std::set<std::string>    covered;
      for (const auto& tgt : targets(query_for(file, home, options.codex_home))) {
        const auto expected = project_for(tgt.layout, file.source_path, file.name);
        if (!expected) {
          continue;
        }
        covered.insert(tgt.target_path);
        const fs::path path{tgt.target_path};
        const auto     current = state_of(path, tgt.layout, *expected);
        std::string    stamp   = options.now;
        if (current == state::equal) {
          if (const auto prev = prior_stamps.find(tgt.target_path); prev != prior_stamps.end() && !prev->second.empty()) {
            stamp = prev->second;
          }
        } else if (current == state::absent) {
          missing.push_back(tgt.target_path);
          writes.push_back({path, tgt.layout, *expected, row_kind});
        } else {
          if (!is_owned(path, home, recorded)) {
            return std::unexpected(reconcile_error{cause::conflict, conflict_path(path, tgt.layout, *expected)});
          }
          stale.push_back(tgt.target_path);
          writes.push_back({path, tgt.layout, *expected, row_kind});
        }
        entry.links.push_back({tgt.vendor, tgt.target_path, file.source_path, manifest::mode::copy, std::move(stamp)});
      }
      // Records this pass does not own or recompute (a vendor no longer present) stay as they were.
      if (old_entry != nullptr) {
        for (const auto& row : old_entry->links) {
          if (!is_legacy(row) && !covered.contains(row.target_path)) {
            entry.links.push_back(row);
          }
        }
      }

      if (!legacy_removed.empty()) {
        actions.push_back({file.name, row_kind, "legacy", file.source_path, std::move(legacy_removed)});
      }
      if (!stale.empty()) {
        actions.push_back({file.name, row_kind, "stale", file.source_path, std::move(stale)});
      }
      if (!missing.empty()) {
        actions.push_back({file.name, row_kind, "target-missing", file.source_path, std::move(missing)});
      }
      computed_order.push_back(file.name);
      computed.emplace(file.name, std::move(entry));
    }

    manifest::link_manifest next{1, {}};
    for (const auto& entry : old_manifest.entries) {
      if (!path_exists(fs::path{entry.source_path})) {
        continue;
      }
      if (const auto it = computed.find(entry.name); it != computed.end()) {
        next.entries.push_back(it->second);
        computed.erase(it);
      } else {
        next.entries.push_back(entry);
      }
    }
    for (const auto& name : computed_order) {
      if (const auto it = computed.find(name); it != computed.end()) {
        next.entries.push_back(it->second);
      }
    }
    after.emplace(row_kind, std::move(next));
  }

  if (!options.dry_run) {
    for (const auto& path : removals) {
      remove_path_if_exists(path);
    }
    // A write that failed is not recorded: its row reverts to the prior one, or is dropped.
    std::set<std::pair<manifest::kind, std::string>> failed;
    for (const auto& write : writes) {
      if (!install(write.path, write.lay, write.content)) {
        failed.emplace(write.kind, write.path.string());
      }
    }
    if (!failed.empty()) {
      std::vector<std::string> paths;
      for (auto& [row_kind, bad] : failed) {
        paths.push_back(bad);
        auto& entries = after.at(row_kind).entries;
        for (auto& entry : entries) {
          const manifest::manifest_record* prior = nullptr;
          for (const auto& old_entry : before.at(row_kind).entries) {
            for (const auto& row : old_entry.links) {
              if (old_entry.name == entry.name && row.target_path == bad) {
                prior = &row;
              }
            }
          }
          for (auto it = entry.links.begin(); it != entry.links.end();) {
            if (it->target_path != bad) {
              ++it;
            } else if (prior != nullptr) {
              *it = *prior;
              ++it;
            } else {
              it = entry.links.erase(it);
            }
          }
        }
      }
      actions.push_back({"", manifest::kind::skill, "write-failed", "", std::move(paths)});
    }
    for (const auto row_kind : {manifest::kind::skill, manifest::kind::agent}) {
      save_if_changed(home, row_kind, before.at(row_kind), after.at(row_kind));
    }
  }
  return actions;
}

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

} // namespace planar::engine::local::link
