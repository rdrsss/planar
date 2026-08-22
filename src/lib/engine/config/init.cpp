/// @file init.cpp
/// @brief Implementation of `planar.engine.config.init` (see init.cppm).

module;

module planar.engine.config.init;

import std;
import planar.db;

namespace planar::engine::config {

namespace {

/// @brief Basename of a filesystem path (no trailing slash handling
/// beyond what `std::filesystem::path` already does — `cwd` is validated
/// absolute before this is called).
auto basename_of(std::string_view path) -> std::string {
  return std::filesystem::path(path).filename().string();
}

} // namespace

auto derive_slug(std::string_view name) -> std::string {
  std::string out;
  bool        prev_dash = true; // trims a leading '-'
  for (const unsigned char raw : name) {
    const unsigned char ch = static_cast<unsigned char>(std::tolower(raw));
    if (std::isalnum(ch) != 0) {
      out.push_back(static_cast<char>(ch));
      prev_dash = false;
    } else if (!prev_dash) {
      out.push_back('-');
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "project";
  }
  return out;
}

auto register_cwd(db::connection& conn, const register_cwd_args& args) -> std::expected<registered_project, init_error> {
  if (args.cwd.empty() || args.cwd.front() != '/') {
    return std::unexpected(init_error::invalid_path);
  }

  const std::string base = basename_of(args.cwd);
  const std::string slug = args.slug.value_or(derive_slug(base));
  const std::string name = args.name.value_or(base);

  const std::string_view sql =
      args.force ? std::string_view{"insert into projects (slug, name, root_path, git_remote) "
                                    "values (?, ?, ?, ?) "
                                    "on conflict (slug) do update set "
                                    "name = excluded.name, "
                                    "root_path = excluded.root_path, "
                                    "git_remote = excluded.git_remote, "
                                    "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')"}
                 : std::string_view{"insert or ignore into projects (slug, name, root_path, git_remote) values (?, ?, ?, ?)"};

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(init_error::query_failed);
  }
  if (auto b1 = stmt->bind_text(1, slug); !b1) {
    return std::unexpected(init_error::query_failed);
  }
  if (auto b2 = stmt->bind_text(2, name); !b2) {
    return std::unexpected(init_error::query_failed);
  }
  if (auto b3 = stmt->bind_text(3, args.cwd); !b3) {
    return std::unexpected(init_error::query_failed);
  }
  auto bind_remote = args.git_remote.has_value() ? stmt->bind_text(4, *args.git_remote) : stmt->bind_null(4);
  if (!bind_remote) {
    return std::unexpected(init_error::query_failed);
  }
  if (auto step = stmt->step(); !step) {
    return std::unexpected(init_error::query_failed);
  }

  auto read_stmt =
      conn.prepare("select id, slug, name, root_path, git_remote, created_at, updated_at from projects where slug = ?");
  if (!read_stmt) {
    return std::unexpected(init_error::query_failed);
  }
  if (auto bound = read_stmt->bind_text(1, slug); !bound) {
    return std::unexpected(init_error::query_failed);
  }
  auto step = read_stmt->step();
  if (!step) {
    return std::unexpected(init_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(init_error::query_failed); // unreachable — we just inserted (or it already existed).
  }

  return registered_project{
      .id         = read_stmt->column_int64(0),
      .slug       = read_stmt->column_text(1),
      .name       = read_stmt->column_text(2),
      .root_path  = read_stmt->is_null(3) ? std::optional<std::string>{} : std::optional<std::string>{read_stmt->column_text(3)},
      .git_remote = read_stmt->is_null(4) ? std::optional<std::string>{} : std::optional<std::string>{read_stmt->column_text(4)},
      .created_at = read_stmt->column_text(5),
      .updated_at = read_stmt->column_text(6),
  };
}

} // namespace planar::engine::config
