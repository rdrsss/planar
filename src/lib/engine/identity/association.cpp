/// @file association.cpp
/// @brief Implementation of `planar.engine.identity.association` (see
/// association.cppm).

module;

module planar.engine.identity.association;

import std;
import planar.db;
import planar.git;
import planar.json_text;
import planar.log;
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
/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>` diagnostic
/// ahead of the outer handler error. See
/// `zig/src/engine/identity/association.zig`'s `create`/`addMember` for the
/// shapes this ports; mirrors `engine::planning::exec_failed` (task.cpp).
/// NOTE: does NOT cover `findOrCreateProjectByPath`/`...WithSuffix`, whose
/// oracle diagnostics are "project insert failed"/"project insert (suffix)
/// failed" -- a different message shape, out of this task's 28-site scope.
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> association_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return association_error::query_failed;
}

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
    return std::unexpected(exec_failed("association.create", "PrepareFailed"));
  }
  if (auto b1 = stmt->bind_text(1, args.slug); !b1) {
    return std::unexpected(exec_failed("association.create", "BindFailed"));
  }
  if (auto b2 = stmt->bind_text(2, name); !b2) {
    return std::unexpected(exec_failed("association.create", "BindFailed"));
  }
  if (auto b3 = stmt->bind_text(3, association_kind_to_text(kind)); !b3) {
    return std::unexpected(exec_failed("association.create", "BindFailed"));
  }
  auto bind_config = args.config_json.has_value() ? stmt->bind_text(4, *args.config_json) : stmt->bind_null(4);
  if (!bind_config) {
    return std::unexpected(exec_failed("association.create", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(association_error::slug_conflict);
    }
    return std::unexpected(exec_failed("association.create", "StepFailed"));
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
  // `list_all` is `list` with no predicate, and composing it that way
  // rather than keeping a second copy of the statement is what makes the
  // two provably agree on the ordering.
  return list(conn, list_filter{});
}

auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<association>, association_error> {
  // `where 1 = 1` so the optional term appends unconditionally, exactly as
  // the oracle composes it. It costs nothing and it is the difference
  // between one statement shape and two.
  std::string sql = std::format("{} where 1 = 1", k_select_columns);
  if (filter.kind.has_value()) {
    sql += " and kind = ?";
  }
  sql += " order by slug";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(association_error::query_failed);
  }
  if (filter.kind.has_value()) {
    // The WIRE form (`ad-hoc`), not the enumerator's spelling — see this
    // function's declaration.
    if (auto bound = stmt->bind_text(1, association_kind_to_text(*filter.kind)); !bound) {
      return std::unexpected(association_error::query_failed);
    }
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
    return std::unexpected(exec_failed("association.addMember", "PrepareFailed"));
  }
  if (auto b1 = stmt->bind_int64(1, project->id); !b1) {
    return std::unexpected(exec_failed("association.addMember", "BindFailed"));
  }
  if (auto b2 = stmt->bind_int64(2, assoc->id); !b2) {
    return std::unexpected(exec_failed("association.addMember", "BindFailed"));
  }
  if (auto b3 = stmt->bind_text(3, add_member_source_to_text(source)); !b3) {
    return std::unexpected(exec_failed("association.addMember", "BindFailed"));
  }
  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(association_error::already_member);
    }
    return std::unexpected(exec_failed("association.addMember", "StepFailed"));
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

auto render_list_text(std::span<const association> items) -> std::string {
  if (items.empty()) {
    // `(no associations)`, not the member renderer's `(no members)`.
    return "(no associations)\n";
  }
  std::string out;
  for (const auto& a : items) {
    // Third column is `name`. Second is the kind at width TWELVE.
    out += std::format("{:<20}  {:<12}  {}\n", a.slug, association_kind_to_text(a.kind), a.name);
  }
  return out;
}

auto render_list_json(std::span<const association> items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
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

// ===========================================================================
// Auto-detection (proposals, enrichment, apply) — plan 996, task 6325
// ===========================================================================

namespace {

/// @brief Strip `suffix` from `s` if present. Mirrors zig's `trimSuffix`.
auto trim_suffix(std::string_view s, std::string_view suffix) -> std::string_view {
  if (s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix) {
    return s.substr(0, s.size() - suffix.size());
  }
  return s;
}

/// @brief Lowercase, keep `[a-z0-9._-]`, collapse every other run to a
/// single dash, then strip trailing dashes. Mirrors zig's
/// `sanitizeHostSlug`.
///
/// This is NOT this file's existing `slugify_path_segment`, and the
/// difference is not cosmetic. That helper keeps ALNUM ONLY and falls back
/// to the literal `"_"` on empty input; this one additionally preserves
/// `.`, `_` and `-`, and returns EMPTY when nothing survives. Reusing it
/// here would turn `host:my_host.example.com` into `host:my-host-example-com`
/// — a different slug, a different row, silently. Probed against the
/// oracle: `HOST..com` stays `host..com`, doubled dot and all.
///
/// The dash-collapse state starts as "already dashed" so leading junk
/// produces no leading dash, and a kept literal `-` re-arms it so `a--b`
/// survives verbatim rather than collapsing.
auto sanitize_host_slug(std::string_view s) -> std::string {
  std::string out;
  bool        prev_dash = true;
  for (const unsigned char raw : s) {
    const auto lower = static_cast<unsigned char>(std::tolower(raw));
    const bool keep  = (std::isalnum(lower) != 0) || lower == '.' || lower == '_' || lower == '-';
    if (keep) {
      out.push_back(static_cast<char>(lower));
      prev_dash = (lower == '-');
    } else if (!prev_dash) {
      out.push_back('-');
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

/// @brief Lowercase, keep `[a-z0-9_-]`, collapse every other run to a
/// single dash, then strip trailing dashes. Mirrors zig's
/// `sanitizeSlugPart`.
///
/// Differs from `sanitize_host_slug` by exactly one character class: `.`
/// is NOT kept here, so a parent directory named `dot.name` becomes
/// `dot-name` while a HOST named `dot.name` stays `dot.name`. Both were
/// probed. It differs from `slugify_path_segment` in the same two ways
/// that helper's sibling does — `_`/`-` preserved, empty stays empty — so
/// `a__b` and `a--b` survive intact and a directory named `+++` sanitizes
/// to nothing rather than to `_`.
auto sanitize_slug_part(std::string_view s) -> std::string {
  std::string out;
  bool        prev_dash = true;
  for (const unsigned char raw : s) {
    const auto lower = static_cast<unsigned char>(std::tolower(raw));
    const bool keep  = (std::isalnum(lower) != 0) || lower == '_' || lower == '-';
    if (keep) {
      out.push_back(static_cast<char>(lower));
      prev_dash = (lower == '-');
    } else if (!prev_dash) {
      out.push_back('-');
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

/// @brief The ecosystem marker files, in the order the oracle tests them.
///
/// The ORDER is the tie-break and it is not alphabetical, not
/// most-specific-first, and not stable under reordering: a repository
/// carrying `go.mod` AND `Cargo.toml` AND `package.json` AND
/// `pyproject.toml` resolves to `go` purely because `go.mod` is tested
/// first. Probed against the oracle with exactly that four-marker fixture.
/// Only ONE proposal is ever emitted — the search returns on first hit
/// rather than accumulating.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> k_lang_markers{{
    {"go.mod", "go"},
    {"Cargo.toml", "rust"},
    {"package.json", "javascript"},
    {"pyproject.toml", "python"},
}};

/// @brief Return the ecosystem tag for `dir`, or unset.
///
/// Top-level only — deliberately conservative, matching zig's `detectLang`.
/// A `go.mod` one directory down does not register.
auto detect_lang(const std::filesystem::path& dir) -> std::optional<std::string> {
  for (const auto& [file, lang] : k_lang_markers) {
    std::error_code ec;
    // `exists` and not `is_regular_file`: the oracle's probe is
    // `Dir.accessAbsolute`, which succeeds for a DIRECTORY named `go.mod`
    // too. The error_code overload is used so a permission failure on one
    // candidate skips it rather than throwing.
    if (std::filesystem::exists(dir / file, ec) && !ec) {
      return std::string{lang};
    }
  }
  return std::nullopt;
}

/// @brief Resolve `root_path` to a `projects.id`, or zero when no project
/// is registered there. Zero is a sentinel the oracle uses too.
auto lookup_project_id(db::connection& conn, std::string_view root_path) -> std::expected<std::int64_t, association_error> {
  auto stmt = conn.prepare("select id from projects where root_path = ?");
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
  if (*step == db::step_result::done) {
    return 0;
  }
  return stmt->column_int64(0);
}

} // namespace

auto parse_remote(std::string_view remote) -> remote_parts {
  // SCP-style `git@host:owner/repo.git`. Matched on the literal prefix, so
  // `ssh://git@host/owner/repo.git` deliberately falls through to the
  // URL branch below (probed: it yields host `github.com`, owner `owner`).
  constexpr std::string_view k_scp_prefix = "git@";
  if (remote.starts_with(k_scp_prefix)) {
    auto const rest  = remote.substr(k_scp_prefix.size());
    auto const colon = rest.find(':');
    if (colon == std::string_view::npos) {
      // A bare `git@github.com` names no repository. Probed: no proposals.
      return {};
    }
    auto const host      = rest.substr(0, colon);
    auto const path      = rest.substr(colon + 1);
    auto const slash     = path.find('/');
    auto const owner_raw = slash == std::string_view::npos ? path : path.substr(0, slash);
    return {.host = host, .owner = trim_suffix(owner_raw, ".git")};
  }

  // URL-style `scheme://[user@]host[:port]/owner/repo.git`. No `://` at
  // all means an unrecognized remote (a local path, say) and BOTH fields
  // come back empty, which produces no proposals rather than a guess.
  auto const scheme_end = remote.find("://");
  if (scheme_end == std::string_view::npos) {
    return {};
  }
  auto rest = remote.substr(scheme_end + 3);
  if (auto const at = rest.find('@'); at != std::string_view::npos) {
    rest = rest.substr(at + 1);
  }
  auto const slash_idx = rest.find('/');
  if (slash_idx == std::string_view::npos) {
    return {};
  }
  auto host = rest.substr(0, slash_idx);
  if (auto const colon = host.find(':'); colon != std::string_view::npos) {
    host = host.substr(0, colon);
  }
  auto const path = rest.substr(slash_idx + 1);
  if (path.empty()) {
    // `https://github.com/` — a host with no owner. The HOST proposal
    // still fires; only the org one is dropped. Probed.
    return {.host = host, .owner = {}};
  }
  auto const next_slash = path.find('/');
  auto const owner_raw  = next_slash == std::string_view::npos ? path : path.substr(0, next_slash);
  return {.host = host, .owner = trim_suffix(owner_raw, ".git")};
}

auto proposals_from_signals(const detect_signals& sig) -> std::vector<proposal> {
  std::vector<proposal> out;

  // ORACLE ORDER: host, org, path, lang. Append order, not sorted.
  if (sig.git_remote.has_value()) {
    auto const parts = parse_remote(*sig.git_remote);
    if (!parts.host.empty()) {
      out.push_back(proposal{.slug   = std::format("host:{}", sanitize_host_slug(parts.host)),
                             .kind   = association_kind::host,
                             .source = add_member_source::auto_git_remote,
                             .reason = "from git remote host"});
    }
    if (!parts.owner.empty()) {
      out.push_back(proposal{.slug   = std::format("org:{}", sanitize_slug_part(parts.owner)),
                             .kind   = association_kind::org,
                             .source = add_member_source::auto_git_remote,
                             .reason = "from git remote org"});
    }
  }

  if (sig.parent_basename.has_value()) {
    auto const& raw = *sig.parent_basename;
    if (!raw.empty() && raw != "." && raw != "/") {
      // The sanitized part is re-tested for emptiness AFTER sanitizing: a
      // directory named `+++` passes the literal checks above and still
      // must not produce a bare `path:`. Probed — the oracle emits nothing.
      auto part = sanitize_slug_part(raw);
      if (!part.empty()) {
        out.push_back(proposal{.slug   = std::format("path:{}", part),
                               .kind   = association_kind::path,
                               .source = add_member_source::auto_path,
                               .reason = "from parent directory"});
      }
    }
  }

  if (sig.lang.has_value() && !sig.lang->empty()) {
    // The lang tag is NOT sanitized — it is one of four literals this code
    // chose itself, never operator input.
    out.push_back(proposal{.slug   = std::format("lang:{}", *sig.lang),
                           .kind   = association_kind::lang,
                           .source = add_member_source::auto_lang,
                           .reason = "from detected language ecosystem"});
  }

  return out;
}

auto detect_proposals(const std::filesystem::path& dir) -> std::vector<proposal> {
  detect_signals sig;
  sig.git_remote = git::probe_origin_url(dir);

  // The PARENT's basename, not `dir`'s. `path_basename` rather than
  // `std::filesystem::path::filename` for the trailing-separator reason
  // documented on that helper. A path with no parent (`/`) contributes
  // nothing, which is the only way the whole result comes back empty.
  auto const dir_str = dir.string();
  if (auto const slash = dir_str.find_last_of('/'); slash != std::string::npos && slash > 0) {
    sig.parent_basename = path_basename(std::string_view{dir_str}.substr(0, slash));
  }

  sig.lang = detect_lang(dir);
  return proposals_from_signals(sig);
}

auto enrich_proposals(db::connection& conn, std::span<proposal> proposals, std::string_view root_path)
    -> std::expected<void, association_error> {
  // An unregistered root_path is NOT an error here — id zero just means the
  // membership probe below is skipped. `apply_proposals` refuses the same
  // state; this one previews it.
  auto project_id = lookup_project_id(conn, root_path);
  if (!project_id) {
    return std::unexpected(project_id.error());
  }

  for (auto& p : proposals) {
    // TASK 6327: CLEAR both flags before probing, so this function COMPUTES
    // them rather than only ever setting them. The reusing caller is real,
    // not hypothetical: `assoc.cpp`'s `--apply` path calls this a SECOND
    // time on the SAME span after applying (the oracle's `catch {}`
    // re-enrich). With set-only semantics a flag raised on the first pass
    // could never come back down, so a proposal whose row disappeared
    // between passes would keep reporting `already a member`.
    //
    // No behaviour change for either current caller -- between the two
    // passes rows are only ever created -- which is exactly why this was
    // filed as a latent trap rather than a live defect. It is cheap to make
    // the contract match the name.
    p.assoc_exists  = false;
    p.member_exists = false;

    auto stmt = conn.prepare("select id from associations where slug = ?");
    if (!stmt) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, p.slug); !bound) {
      return std::unexpected(association_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(association_error::query_failed);
    }
    if (*step == db::step_result::done) {
      // No association row: BOTH flags stay false. `member_exists` is not
      // even probed, because a membership cannot exist without one.
      continue;
    }
    auto const assoc_id = stmt->column_int64(0);
    p.assoc_exists      = true;
    if (*project_id == 0) {
      continue;
    }

    auto count = conn.prepare("select count(*) from project_associations where project_id = ? and association_id = ?");
    if (!count) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto bound = count->bind_int64(1, *project_id); !bound) {
      return std::unexpected(association_error::query_failed);
    }
    if (auto bound = count->bind_int64(2, assoc_id); !bound) {
      return std::unexpected(association_error::query_failed);
    }
    auto count_step = count->step();
    if (!count_step) {
      return std::unexpected(association_error::query_failed);
    }
    if (*count_step != db::step_result::done) {
      p.member_exists = count->column_int64(0) > 0;
    }
  }
  return {};
}

auto apply_proposals(db::connection& conn, std::span<const proposal> proposals, std::string_view root_path)
    -> std::expected<void, association_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(association_error::query_failed);
  }

  // The REFUSAL comes first, and it precedes the loop — so an empty
  // proposal set against an unregistered project still fails `not_found`
  // rather than succeeding vacuously. Probed against the oracle at `/`.
  auto project_id = lookup_project_id(conn, root_path);
  if (!project_id) {
    return std::unexpected(project_id.error());
  }
  if (*project_id == 0) {
    return std::unexpected(association_error::not_found);
  }

  for (const auto& p : proposals) {
    // `insert or ignore`, NOT an upsert: an association a human created
    // keeps its own `name` and its `auto_detected = 0`. Only a row this
    // statement actually inserts gets `auto_detected = 1` and name == slug.
    {
      auto stmt = conn.prepare("insert or ignore into associations (slug, name, kind, auto_detected) values (?, ?, ?, 1)");
      if (!stmt) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_text(1, p.slug); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_text(2, p.slug); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_text(3, association_kind_to_text(p.kind)); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto step = stmt->step(); !step) {
        return std::unexpected(association_error::query_failed);
      }
    }

    // Re-read the id rather than using last_insert_rowid: on the
    // `or ignore` path nothing was inserted and that rowid would name an
    // unrelated row.
    std::int64_t assoc_id = 0;
    {
      auto stmt = conn.prepare("select id from associations where slug = ?");
      if (!stmt) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_text(1, p.slug); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(association_error::query_failed);
      }
      if (*step == db::step_result::done) {
        // The row we just ensured is missing. The oracle reports this as a
        // generic query failure, not `not_found`.
        return std::unexpected(association_error::query_failed);
      }
      assoc_id = stmt->column_int64(0);
    }

    // The membership row DOES upsert, so a re-detection refreshes `source`
    // to the current `auto:*` value. This is the one place the two
    // statements' conflict policies differ, and it is deliberate.
    {
      auto stmt = conn.prepare("insert into project_associations (project_id, association_id, source) values (?, ?, ?) "
                               "on conflict (project_id, association_id) do update set source = excluded.source");
      if (!stmt) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_int64(1, *project_id); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_int64(2, assoc_id); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto bound = stmt->bind_text(3, add_member_source_to_text(p.source)); !bound) {
        return std::unexpected(association_error::query_failed);
      }
      if (auto step = stmt->step(); !step) {
        return std::unexpected(association_error::query_failed);
      }
    }

    // ORACLE: verb `link`, entity kind `association` with the ASSOCIATION's
    // id, and a summary carrying a literal U+2192 RIGHTWARDS ARROW -- not
    // an ASCII `->`. This is the fourth and last of the Zig module's audit
    // call sites.
    if (auto a = record_audit(
            conn, audit::record_args{.verb    = audit::verb::link,
                                     .entity  = {.kind = "association", .id = assoc_id},
                                     .summary = std::format("auto-link project_id={} → association '{}'", *project_id, p.slug)});
        !a) {
      return std::unexpected(a.error());
    }
  }

  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(association_error::query_failed);
  }
  return {};
}

auto proposal_action_label(const proposal& p) -> std::string_view {
  // Most-specific first: membership implies the association exists, so
  // testing `assoc_exists` first would mislabel every existing member.
  if (p.member_exists) {
    return "already a member";
  }
  if (p.assoc_exists) {
    return "already exists, will add";
  }
  return "will create";
}

auto render_detect_text(std::span<const proposal> proposals) -> std::string {
  if (proposals.empty()) {
    return "no proposed associations\n";
  }
  std::string out = "proposed associations:\n";
  for (const auto& p : proposals) {
    // ORACLE, byte for byte: `"  {s:<24} ({s})  [{s}]\n"`. The spacing is
    // ASYMMETRIC and that is not a typo to tidy -- ONE space between the
    // padded slug and `(`, TWO between `)` and `[`. Transcribed as two-and-
    // two first; the differential run against the oracle caught it. Since
    // the pad already supplies trailing spaces for any slug shorter than
    // 24, the single space is invisible on short slugs and only shows up
    // once a slug reaches the field width.
    out += std::format("  {:<24} ({})  [{}]\n", p.slug, p.reason, proposal_action_label(p));
  }
  return out;
}

auto render_detect_json(std::span<const proposal> proposals) -> std::string {
  // NDJSON, INCLUDING WHEN EMPTY (task 6326). There is no empty branch: the
  // loop runs zero times and the renderer returns zero bytes.
  //
  // It used to short-circuit to `{"proposals":[]}` -- one object naming a
  // key the non-empty payload NEVER emits, wrapping an array it never
  // produces. Two shapes behind one flag, reproduced from the oracle under
  // D2. Decision 1090 authorises the change for this row -- 6326 is one of
  // the eight it names -- on the reasoning 1067 applied to its own nine:
  // with the oracle deleted, D2's bug-for-bug rule no longer decides
  // divergences with real consequences. The family-wide rule (6257 / 6270 / 6326) is N lines
  // for N results with N allowed to be zero. Every line is terminated here,
  // the last one included, so the renderer owns its terminators and the
  // caller appends nothing -- which is what makes an empty render genuinely
  // empty rather than a lone newline.
  std::string out;
  for (std::size_t i = 0; i < proposals.size(); ++i) {
    const auto& p = proposals[i];
    out += std::format(R"({{"slug":{},"kind":"{}","source":"{}","reason":{},"assoc_exists":{},"member_exists":{},"action":{}}})",
                       json_string(p.slug), association_kind_to_text(p.kind), add_member_source_to_text(p.source),
                       json_string(p.reason), p.assoc_exists ? "true" : "false", p.member_exists ? "true" : "false",
                       json_string(proposal_action_label(p)));
    out += "\n";
  }
  return out;
}

} // namespace planar::engine::identity
