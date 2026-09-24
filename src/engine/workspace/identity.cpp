/// @file identity.cpp
/// @brief Implementation of `planar.engine.workspace.identity` (plan 996, task
/// 6110). See identity.cppm for the selection rules, the HOME-safety
/// correction, and the symlink-install fallback.

module;

#include <glaze/glaze.hpp>

module planar.engine.workspace.identity;

import std;
import planar.db;

namespace planar::engine::workspace::identity {

namespace {

auto trim(std::string_view text) -> std::string_view {
  constexpr std::string_view chars = " \t\r\n";
  const auto                 first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(chars) - first + 1);
}

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

auto read_link(const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
  std::error_code ec;
  auto            target = std::filesystem::read_symlink(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return target;
}

/// @brief Read one org row off a stepped statement.
///
/// Column order is fixed by every caller's SELECT: id, slug, name,
/// coalesce(config_json, '').
auto read_workspace(db::statement& stmt) -> workspace {
  return workspace{stmt.column_int64(0), stmt.column_text(1), stmt.column_text(2), parse_root_path(stmt.column_text(3))};
}

auto resolve_by_id(db::connection& conn, std::int64_t org_id) -> std::expected<workspace, resolve_error> {
  auto stmt = conn.prepare("select id, slug, name, coalesce(config_json, '') from associations "
                           "where kind = 'org' and id = ?");
  if (!stmt || !stmt->bind_int64(1, org_id)) {
    return std::unexpected(resolve_error::query_failed);
  }
  const auto step = stmt->step();
  if (!step) {
    return std::unexpected(resolve_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::unexpected(resolve_error::not_found);
  }
  return read_workspace(*stmt);
}

auto resolve_by_slug(db::connection& conn, std::string_view slug) -> std::expected<workspace, resolve_error> {
  auto stmt = conn.prepare("select id, slug, name, coalesce(config_json, '') from associations "
                           "where kind = 'org' and slug = ?");
  if (!stmt || !stmt->bind_text(1, slug)) {
    return std::unexpected(resolve_error::query_failed);
  }
  const auto step = stmt->step();
  if (!step) {
    return std::unexpected(resolve_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::unexpected(resolve_error::not_found);
  }
  return read_workspace(*stmt);
}

/// @brief Install one guidance link; true when it fell back to a byte copy.
auto install_one(const std::filesystem::path& link_path, const std::filesystem::path& target) -> std::optional<bool> {
  if (const auto existing = read_link(link_path); existing && *existing == target) {
    // Already correct: left completely alone rather than removed and
    // recreated. This is what makes doctor idempotent.
    return false;
  }

  const auto      parent = link_path.has_parent_path() ? link_path.parent_path() : std::filesystem::path{"."};
  const auto      tmp    = parent / std::format(".{}.symlink.tmp", link_path.filename().string());
  std::error_code ec;
  std::filesystem::remove(tmp, ec);

  ec.clear();
  std::filesystem::create_symlink(target, tmp, ec);
  if (ec) {
    // The filesystem refused a symlink (a read-only mount, Windows without
    // developer mode). Copy the BYTES instead — the guidance is still correct,
    // it just stops tracking regeneration.
    std::ifstream in(target, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        return std::nullopt;
      }
      out << in.rdbuf();
      out.flush();
      if (!out || in.bad()) {
        return std::nullopt;
      }
    }
    ec.clear();
    std::filesystem::rename(tmp, link_path, ec);
    if (ec) {
      std::error_code rmec;
      std::filesystem::remove(tmp, rmec);
      return std::nullopt;
    }
    return true;
  }

  ec.clear();
  std::filesystem::rename(tmp, link_path, ec);
  if (ec) {
    std::error_code rmec;
    std::filesystem::remove(tmp, rmec);
    return std::nullopt;
  }
  return false;
}

} // namespace

auto parse_root_path(std::string_view config_json) -> std::optional<std::string> {
  if (config_json.empty()) {
    return std::nullopt;
  }
  glz::generic parsed{};
  if (glz::read_json(parsed, config_json)) {
    // Malformed config is NOT an error here — the row is still a usable
    // workspace, it just has no root to install guidance into.
    return std::nullopt;
  }
  if (!parsed.is_object()) {
    return std::nullopt;
  }
  const auto& obj = parsed.get_object();
  const auto  it  = obj.find("root_path");
  if (it == obj.end() || !it->second.is_string()) {
    return std::nullopt;
  }
  auto value = it->second.get_string();
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

auto resolve_org(db::connection& conn, std::optional<std::string_view> target) -> std::expected<workspace, resolve_error> {
  const auto raw = target ? trim(*target) : std::string_view{};

  if (raw.empty()) {
    // `limit 2` is the whole ambiguity check: two rows is enough to know there
    // is more than one, and there is no reason to read the rest.
    auto stmt = conn.prepare("select id, slug, name, coalesce(config_json, '') from associations "
                             "where kind = 'org' order by id limit 2");
    if (!stmt) {
      return std::unexpected(resolve_error::query_failed);
    }
    std::optional<workspace> first;
    std::size_t              count = 0;
    while (true) {
      const auto step = stmt->step();
      if (!step) {
        return std::unexpected(resolve_error::query_failed);
      }
      if (*step == db::step_result::done) {
        break;
      }
      ++count;
      if (count == 1) {
        first = read_workspace(*stmt);
      }
    }
    if (count == 0) {
      return std::unexpected(resolve_error::not_found);
    }
    if (count > 1) {
      return std::unexpected(resolve_error::ambiguous);
    }
    return *first;
  }

  const auto   value  = raw.starts_with("org:") ? raw.substr(4) : raw;
  std::int64_t org_id = 0;
  const auto*  last   = value.data() + value.size();
  const auto   conv   = std::from_chars(value.data(), last, org_id);
  if (conv.ec == std::errc{} && conv.ptr == last) {
    return resolve_by_id(conn, org_id);
  }
  return resolve_by_slug(conn, value);
}

auto list_orgs(db::connection& conn) -> std::optional<std::vector<workspace>> {
  auto stmt = conn.prepare("select id, slug, name, coalesce(config_json, '') from associations "
                           "where kind = 'org' order by id");
  if (!stmt) {
    return std::nullopt;
  }
  std::vector<workspace> out;
  while (true) {
    const auto step = stmt->step();
    if (!step) {
      return std::nullopt;
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(read_workspace(*stmt));
  }
  return out;
}

auto planar_home(const env_lookup& env) -> std::optional<std::filesystem::path> {
  const auto home = env("HOME");
  if (const auto raw = env("PLANAR_HOME"); raw && !raw->empty()) {
    // `~` expansion, which only ever fires on these two exact shapes: a `~`
    // in the middle of a path is a literal character, not a home reference.
    if (*raw == "~") {
      if (!home) {
        return std::nullopt;
      }
      return std::filesystem::path{*home};
    }
    if (raw->starts_with("~/")) {
      if (!home) {
        return std::nullopt;
      }
      return std::filesystem::path{*home} / raw->substr(2);
    }
    return std::filesystem::path{*raw};
  }
  if (!home) {
    return std::nullopt;
  }
  return std::filesystem::path{*home} / ".planar";
}

auto load_layout(const env_lookup& env, std::int64_t org_id) -> std::optional<layout> {
  if (org_id <= 0) {
    return std::nullopt;
  }
  const auto home = planar_home(env);
  if (!home) {
    return std::nullopt;
  }
  const auto dir = *home / "workspaces" / std::format("{}", org_id);
  return layout{org_id, dir, dir / "AGENTS.md", dir / "routing-table.json", dir / "config.toml", dir / "README.md"};
}

auto ensure_layout(const env_lookup& env, std::int64_t org_id) -> std::optional<layout> {
  auto value = load_layout(env, org_id);
  if (!value) {
    return std::nullopt;
  }
  std::error_code ec;
  std::filesystem::create_directories(value->dir, ec);
  if (ec && !std::filesystem::is_directory(value->dir, ec)) {
    return std::nullopt;
  }
  return value;
}

auto dirty_links(const std::filesystem::path& workspace_root, const std::filesystem::path& target) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto name : link_names) {
    const auto path = workspace_root / std::string{name};
    if (const auto existing = read_link(path)) {
      if (*existing != target) {
        out.emplace_back(name);
      }
      continue;
    }
    // readlink failed for ANY reason — absent, or a real file an operator
    // wrote by hand. Both are dirty. (The Zig original branches here on
    // whether the path exists and then appends the name in BOTH arms; the
    // branch has no effect and is collapsed. See identity.cppm.)
    out.emplace_back(name);
  }
  return out;
}

auto install_symlinks(const std::filesystem::path& workspace_root, const layout& value) -> std::optional<std::string_view> {
  if (workspace_root.empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  std::filesystem::create_directories(workspace_root, ec);

  bool used_copy = false;
  for (const auto name : link_names) {
    const auto copied = install_one(workspace_root / std::string{name}, value.agents_md);
    if (!copied) {
      return std::nullopt;
    }
    used_copy = used_copy || *copied;
  }
  // ONE fallback makes the whole call report `copy`, even when the other link
  // was a real symlink. The strategy describes the install, not a file.
  return used_copy ? strategy_copy : strategy_symlink;
}

auto remove_symlinks(const std::filesystem::path& workspace_root) -> bool {
  if (workspace_root.empty()) {
    return false;
  }
  for (const auto name : link_names) {
    std::error_code ec;
    std::filesystem::remove(workspace_root / std::string{name}, ec);
    if (ec) {
      return false;
    }
  }
  return true;
}

} // namespace planar::engine::workspace::identity
