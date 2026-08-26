/// @file association.cpp
/// @brief Implementation of `planar.engine.identity.association` (see
/// association.cppm).

module;

module planar.engine.identity.association;

import std;
import planar.db;
import planar.json_text;
import planar.policy;

namespace planar::engine::identity {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the association's own write.
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, association_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(association_error::audit_write_failed);
  }
  return {};
}

// SQLite extended result codes this module distinguishes. Mirrored here
// rather than pulling in <sqlite3.h> — this module never touches the raw C
// API, only `planar.db`'s own typed surface (db.cppm's file comment: the
// raw `sqlite3*`/`sqlite3_stmt*` types never appear outside db.cppm/db.cpp).
constexpr int k_sqlite_constraint_unique     = 2067; // SQLITE_CONSTRAINT_UNIQUE
constexpr int k_sqlite_constraint_primarykey = 1555; // SQLITE_CONSTRAINT_PRIMARYKEY (project_associations' composite PK)

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique || err.code_ == k_sqlite_constraint_primarykey;
}

/// @brief The last path component of `path`, mirroring zig's
/// `std.fs.path.basename` (POSIX form) rather than
/// `std::filesystem::path::filename`.
///
/// The two disagree on exactly one input shape and it is an input an
/// operator can type: a path ending in a separator. `std::filesystem`
/// treats `"/a/b/repo/"` as naming the directory `repo` with an EMPTY
/// filename, so `.filename()` returns `""`; zig strips trailing separators
/// first and returns `"repo"`.
///
/// That single character used to decide what an auto-registered `projects`
/// row is CALLED. Captured by running both binaries over the same argv:
/// `assoc add acme "$PWD/"` wrote `slug='repo-2', name='repo'` on the
/// oracle and `slug='_', name=''` on this port — exit 0 on both sides,
/// indistinguishable stdout, and a row the operator can never find again
/// by name. `slug='_'` is `slugify_path_segment`'s empty-input fallback
/// firing, which is the tell.
///
/// The POSIX algorithm is transcribed, not approximated: strip every
/// trailing `/` (returning empty if that consumes the whole string), then
/// take everything after the last remaining `/`. Windows-native `\`
/// separators are NOT handled, matching `std.fs.path.basenamePosix` — the
/// oracle dispatches on `native_os` and this port is measured against its
/// POSIX build.
/// @param path The path to take the basename of.
/// @return The basename, possibly empty.
///
/// Returns an owning `std::string` rather than a view, and that is not
/// style. `assoc add acme /` legitimately yields an EMPTY basename, which
/// is then bound as `projects.name`. A default-constructed `string_view`
/// has a NULL data pointer, `sqlite3_bind_text(nullptr, 0)` binds SQL
/// NULL rather than `''`, and `projects.name` is NOT NULL — so the verb
/// died with `QueryFailed` where the oracle exits 0 and writes a row with
/// an empty name. Caught by the differential probe; the fix is to never
/// hand a possibly-null view to the binder.
auto path_basename(std::string_view path) -> std::string {
  if (path.empty()) {
    return {};
  }
  std::size_t end = path.size();
  while (end > 0 && path[end - 1] == '/') {
    end -= 1;
  }
  if (end == 0) {
    return {};
  }
  auto const slash = path.substr(0, end).find_last_of('/');
  if (slash == std::string_view::npos) {
    return std::string{path.substr(0, end)};
  }
  return std::string{path.substr(slash + 1, end - slash - 1)};
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

  // `path_basename`, NOT `std::filesystem::path::filename` — see that
  // helper for the trailing-separator divergence it exists to close.
  const auto basename = path_basename(root_path);

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

  const auto id = stmt->column_int64(0);
  // ORACLE: `create|association|1|create association 'project:p'` -- the
  // SLUG, not the name. They differ whenever `--name` is passed, and the
  // default makes them identical, so a fixture that never passes `--name`
  // cannot tell the two apart. Captured against a slug-only fixture AND
  // read off the Zig call site's argument to confirm which one it is.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "association", .id = id},
                                                     .summary = std::format("create association '{}'", args.slug)});
      !a) {
    return std::unexpected(a.error());
  }
  return show_by_id(conn, id);
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
  // ORACLE: verb `link` (not `create`), entity kind `association` with the
  // ASSOCIATION's id -- not the project's -- and the summary
  // `add project 'proj' to association 'project:p'`.
  return record_audit(
      conn, audit::record_args{.verb    = audit::verb::link,
                               .entity  = {.kind = "association", .id = assoc->id},
                               .summary = std::format("add project '{}' to association '{}'", project->slug, assoc->slug)});
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
  // ORACLE: verb `unlink`, and `remove project 'proj' from association
  // 'project:p'` -- "from", where `add_member` says "to".
  return record_audit(
      conn, audit::record_args{.verb    = audit::verb::unlink,
                               .entity  = {.kind = "association", .id = assoc->id},
                               .summary = std::format("remove project '{}' from association '{}'", project->slug, assoc->slug)});
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

auto render_member_list_text(std::span<const project_ref> members) -> std::string {
  if (members.empty()) {
    return "(no members)\n";
  }
  std::string out;
  for (const auto& p : members) {
    // ORACLE: the second column is `root_path`, and an unset one is the
    // literal `(no root)`. See this function's doc comment for the fixture
    // that separated `root_path` from the equally-plausible `name`.
    out += std::format("{:<20}  {}\n", p.slug, p.root_path.value_or(std::string{"(no root)"}));
  }
  return out;
}

auto render_member_list_json(std::span<const project_ref> members) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    const auto& p = members[i];
    out += std::format(R"({{"id":{},"slug":{},"name":{},"root_path":{}}})", p.id, json_string(p.slug), json_string(p.name),
                       p.root_path.has_value() ? json_string(*p.root_path) : std::string{"null"});
  }
  out += "]";
  return out;
}

} // namespace planar::engine::identity
