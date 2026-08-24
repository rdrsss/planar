/// @file association.cpp
/// @brief Implementation of `planar.engine.identity.association` (see
/// association.cppm).

module;

module planar.engine.identity.association;

import std;
import planar.db;
import planar.json_text;

namespace planar::engine::identity {

using json_text::json_string;

namespace {

// SQLite extended result codes this module distinguishes. Mirrored here
// rather than pulling in <sqlite3.h> — this module never touches the raw C
// API, only `planar.db`'s own typed surface (db.cppm's file comment: the
// raw `sqlite3*`/`sqlite3_stmt*` types never appear outside db.cppm/db.cpp).
constexpr int k_sqlite_constraint_unique     = 2067; // SQLITE_CONSTRAINT_UNIQUE
constexpr int k_sqlite_constraint_primarykey = 1555; // SQLITE_CONSTRAINT_PRIMARYKEY (project_associations' composite PK)

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique || err.code_ == k_sqlite_constraint_primarykey;
}

/// @brief lowercase-and-collapse-to-single-dash slugify, mirroring zig's
/// `slugifyPathSegment`.
auto slugify_path_segment(std::string_view s) -> std::string {
  std::string out;
  bool        last_dash = true;
  for (const unsigned char ch : s) {
    const unsigned char lower = static_cast<unsigned char>(std::tolower(ch));
    if (std::isalnum(lower) != 0) {
      out.push_back(static_cast<char>(lower));
      last_dash = false;
    } else if (!last_dash) {
      out.push_back('-');
      last_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  if (out.empty()) {
    out.push_back('_');
  }
  return out;
}

auto read_row(db::statement& stmt) -> std::expected<association, association_error> {
  const auto kind_text = stmt.column_text(3);
  const auto kind      = association_kind_from_text(kind_text);
  if (!kind.has_value()) {
    return std::unexpected(association_error::unknown_kind);
  }
  return association{
      .id            = stmt.column_int64(0),
      .slug          = stmt.column_text(1),
      .name          = stmt.column_text(2),
      .kind          = *kind,
      .auto_detected = stmt.column_int64(4) != 0,
      .config_json   = stmt.is_null(5) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(5)},
      .created_at    = stmt.column_text(6),
      .updated_at    = stmt.column_text(7),
  };
}

constexpr std::string_view k_select_columns =
    "select id, slug, name, kind, auto_detected, config_json, created_at, updated_at from associations";

auto project_by_path(db::connection& conn, std::string_view root_path) -> std::expected<project_ref, association_error> {
  auto stmt = conn.prepare("select id, slug, name, root_path from projects where root_path = ?");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, root_path); !bound) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(association_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(association_error::not_found);
  }
  return project_ref{
      .id        = stmt->column_int64(0),
      .slug      = stmt->column_text(1),
      .name      = stmt->column_text(2),
      .root_path = stmt->is_null(3) ? std::optional<std::string>{} : std::optional<std::string>{stmt->column_text(3)},
  };
}

auto project_by_id(db::connection& conn, std::int64_t id) -> std::expected<project_ref, association_error> {
  auto stmt = conn.prepare("select id, slug, name, root_path from projects where id = ?");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(association_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(association_error::not_found);
  }
  return project_ref{
      .id        = stmt->column_int64(0),
      .slug      = stmt->column_text(1),
      .name      = stmt->column_text(2),
      .root_path = stmt->is_null(3) ? std::optional<std::string>{} : std::optional<std::string>{stmt->column_text(3)},
  };
}

/// @brief Insert a `projects` row for `basename`/`root_path`, retrying
/// with a numeric suffix on a slug collision. Mirrors zig's
/// `findOrCreateProjectByPathWithSuffix`.
auto insert_project_with_suffix(db::connection& conn, std::string_view basename, std::string_view root_path)
    -> std::expected<project_ref, association_error> {
  for (std::uint32_t suffix = 2; suffix < 10000; ++suffix) {
    const auto base_slug = slugify_path_segment(basename);
    const auto candidate = std::format("{}-{}", base_slug, suffix);

    auto stmt = conn.prepare("insert into projects (slug, name, root_path) values (?, ?, ?) returning id");
    if (!stmt) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto b1 = stmt->bind_text(1, candidate); !b1) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto b2 = stmt->bind_text(2, basename); !b2) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto b3 = stmt->bind_text(3, root_path); !b3) {
      return std::unexpected(association_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      if (is_unique_violation(step.error())) {
        continue;
      }
      return std::unexpected(association_error::query_failed);
    }
    return project_by_id(conn, stmt->column_int64(0));
  }
  return std::unexpected(association_error::query_failed); // gave up after 10k attempts
}

/// @brief Look up (or create) the `projects` row at `root_path`. Mirrors
/// zig's `findOrCreateProjectByPath`.
auto find_or_create_project_by_path(db::connection& conn, std::string_view root_path)
    -> std::expected<project_ref, association_error> {
  auto existing = project_by_path(conn, root_path);
  if (existing) {
    return existing;
  }
  if (existing.error() != association_error::not_found) {
    return existing;
  }

  const auto basename = std::filesystem::path(root_path).filename().string();

  auto stmt = conn.prepare("insert into projects (slug, name, root_path) values (?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  const auto slug = slugify_path_segment(basename);
  if (auto b1 = stmt->bind_text(1, slug); !b1) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b2 = stmt->bind_text(2, basename); !b2) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b3 = stmt->bind_text(3, root_path); !b3) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return insert_project_with_suffix(conn, basename, root_path);
    }
    return std::unexpected(association_error::query_failed);
  }
  return project_by_id(conn, stmt->column_int64(0));
}

} // namespace

auto association_kind_from_text(std::string_view s) -> std::optional<association_kind> {
  if (s == "org") {
    return association_kind::org;
  }
  if (s == "project") {
    return association_kind::project;
  }
  if (s == "client") {
    return association_kind::client;
  }
  if (s == "personal") {
    return association_kind::personal;
  }
  if (s == "ad-hoc") {
    return association_kind::ad_hoc;
  }
  if (s == "host") {
    return association_kind::host;
  }
  if (s == "path") {
    return association_kind::path;
  }
  if (s == "lang") {
    return association_kind::lang;
  }
  return std::nullopt;
}

auto association_kind_to_text(association_kind k) -> std::string_view {
  switch (k) {
  case association_kind::org:
    return "org";
  case association_kind::project:
    return "project";
  case association_kind::client:
    return "client";
  case association_kind::personal:
    return "personal";
  case association_kind::ad_hoc:
    return "ad-hoc";
  case association_kind::host:
    return "host";
  case association_kind::path:
    return "path";
  case association_kind::lang:
    return "lang";
  }
  return "ad-hoc"; // unreachable — every enumerator handled above.
}

auto add_member_source_to_text(add_member_source s) -> std::string_view {
  switch (s) {
  case add_member_source::user:
    return "user";
  case add_member_source::auto_git_remote:
    return "auto:git-remote";
  case add_member_source::auto_path:
    return "auto:path";
  case add_member_source::auto_lang:
    return "auto:lang";
  }
  return "user"; // unreachable — every enumerator handled above.
}

auto create(db::connection& conn, const create_args& args) -> std::expected<association, association_error> {
  const std::string name = args.name.value_or(args.slug);
  const auto        kind = args.kind.value_or(association_kind::ad_hoc);

  auto stmt = conn.prepare("insert into associations (slug, name, kind, config_json) values (?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b1 = stmt->bind_text(1, args.slug); !b1) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b2 = stmt->bind_text(2, name); !b2) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b3 = stmt->bind_text(3, association_kind_to_text(kind)); !b3) {
    return std::unexpected(association_error::query_failed);
  }
  auto bind_config = args.config_json.has_value() ? stmt->bind_text(4, *args.config_json) : stmt->bind_null(4);
  if (!bind_config) {
    return std::unexpected(association_error::query_failed);
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(association_error::slug_conflict);
    }
    return std::unexpected(association_error::query_failed);
  }

  return show_by_id(conn, stmt->column_int64(0));
}

auto show_by_id(db::connection& conn, std::int64_t id) -> std::expected<association, association_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(association_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(association_error::not_found);
  }
  return read_row(*stmt);
}

auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<association, association_error> {
  auto stmt = conn.prepare(std::format("{} where slug = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, slug); !bound) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(association_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(association_error::not_found);
  }
  return read_row(*stmt);
}

auto list_all(db::connection& conn) -> std::expected<std::vector<association>, association_error> {
  auto stmt = conn.prepare(std::format("{} order by slug", k_select_columns));
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }

  std::vector<association> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(association_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto add_member(db::connection& conn, std::string_view assoc_slug, std::string_view repo_path, add_member_source source)
    -> std::expected<void, association_error> {
  auto assoc = show_by_slug(conn, assoc_slug);
  if (!assoc) {
    return std::unexpected(assoc.error());
  }
  auto project = find_or_create_project_by_path(conn, repo_path);
  if (!project) {
    return std::unexpected(project.error());
  }

  auto stmt = conn.prepare("insert into project_associations (project_id, association_id, source) values (?, ?, ?)");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b1 = stmt->bind_int64(1, project->id); !b1) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b2 = stmt->bind_int64(2, assoc->id); !b2) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b3 = stmt->bind_text(3, add_member_source_to_text(source)); !b3) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(association_error::already_member);
    }
    return std::unexpected(association_error::query_failed);
  }
  return {};
}

auto remove_member(db::connection& conn, std::string_view assoc_slug, std::string_view repo_path)
    -> std::expected<void, association_error> {
  auto assoc = show_by_slug(conn, assoc_slug);
  if (!assoc) {
    return std::unexpected(assoc.error());
  }
  auto project = project_by_path(conn, repo_path);
  if (!project) {
    if (project.error() == association_error::not_found) {
      return std::unexpected(association_error::not_a_member);
    }
    return std::unexpected(project.error());
  }

  auto stmt = conn.prepare("delete from project_associations where project_id = ? and association_id = ?");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b1 = stmt->bind_int64(1, project->id); !b1) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto b2 = stmt->bind_int64(2, assoc->id); !b2) {
    return std::unexpected(association_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(association_error::query_failed);
  }
  return {};
}

auto members(db::connection& conn, std::string_view assoc_slug) -> std::expected<std::vector<project_ref>, association_error> {
  auto assoc = show_by_slug(conn, assoc_slug);
  if (!assoc) {
    return std::unexpected(assoc.error());
  }

  auto stmt = conn.prepare("select p.id, p.slug, p.name, p.root_path from projects p "
                           "join project_associations pa on pa.project_id = p.id "
                           "where pa.association_id = ? order by p.slug");
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, assoc->id); !bound) {
    return std::unexpected(association_error::query_failed);
  }

  std::vector<project_ref> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(association_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(project_ref{
        .id        = stmt->column_int64(0),
        .slug      = stmt->column_text(1),
        .name      = stmt->column_text(2),
        .root_path = stmt->is_null(3) ? std::optional<std::string>{} : std::optional<std::string>{stmt->column_text(3)},
    });
  }
  return out;
}

auto render_text(const association& a) -> std::string {
  std::string out;
  out += std::format("id:        {}\n", a.id);
  out += std::format("slug:      {}\n", a.slug);
  out += std::format("name:      {}\n", a.name);
  out += std::format("kind:      {}\n", association_kind_to_text(a.kind));
  out += std::format("auto:      {}\n", a.auto_detected ? "yes" : "no");
  if (a.config_json.has_value()) {
    out += std::format("config:    {}\n", *a.config_json);
  }
  out += std::format("created:   {}\n", a.created_at);
  out += std::format("updated:   {}\n", a.updated_at);
  return out;
}

auto render_json(const association& a) -> std::string {
  return std::format(R"({{"id":{},"slug":{},"name":{},"kind":"{}","auto_detected":{},)"
                     R"("config_json":{},"created_at":{},"updated_at":{}}})",
                     a.id, json_string(a.slug), json_string(a.name), association_kind_to_text(a.kind),
                     a.auto_detected ? "true" : "false",
                     a.config_json.has_value() ? json_string(*a.config_json) : std::string{"null"}, json_string(a.created_at),
                     json_string(a.updated_at));
}

} // namespace planar::engine::identity
