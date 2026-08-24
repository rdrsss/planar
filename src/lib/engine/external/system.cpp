/// @file system.cpp
/// @brief Implementation of `planar.engine.external.system`. See system.cppm
/// for the port scope and the asymmetric register-helper defaults.

module planar.engine.external.system;

import std;
import planar.db;

namespace planar::engine::external::system {

namespace {

// SQLITE_CONSTRAINT_UNIQUE — the same constant link.cpp and every other
// bucket uses to detect a UNIQUE violation without string-matching the
// driver's message.
constexpr int k_sqlite_constraint_unique = 2067;

constexpr std::string_view k_columns = "id, kind, slug, base_url, default_project, auth_method, auth_ref, created_at, updated_at";

/// @brief Read a nullable TEXT column.
/// @param stmt The stepped statement.
/// @param index The zero-based column index.
/// @return The value, or unset when the column is SQL NULL.
auto text_opt(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

/// @brief Bind an optional TEXT parameter.
///
/// Explicit `bind_null` rather than leaning on `bind_text`'s behavior for a
/// default-constructed `string_view` (`planar.db`'s `bind_text` forwards a
/// null `data()` and SQLite binds SQL NULL) — the trap link.cpp documents and
/// task 6097 owns the root fix for.
/// @param stmt The statement to bind on.
/// @param index The one-based parameter index.
/// @param value The value, or unset for SQL NULL.
/// @return Whether the bind succeeded.
auto bind_text_opt(db::statement& stmt, int index, const std::optional<std::string>& value) -> bool {
  if (!value.has_value()) {
    return stmt.bind_null(index).has_value();
  }
  return stmt.bind_text(index, *value).has_value();
}

/// @brief Materialize an `external_system` from a stepped row.
///
/// Both enum columns are re-parsed rather than trusted, and an unrecognized
/// value is `query_failed` — the Zig original does the same rather than
/// silently defaulting.
/// @param stmt The statement positioned on a row.
/// @return The row, or `system_error::query_failed`.
auto read_row(const db::statement& stmt) -> std::expected<external_system, system_error> {
  auto const kind = system_kind_from_text(stmt.column_text(1));
  if (!kind.has_value()) {
    return std::unexpected(system_error::query_failed);
  }
  auto const auth = auth_method_from_text(stmt.column_text(5));
  if (!auth.has_value()) {
    return std::unexpected(system_error::query_failed);
  }
  return external_system{
      .id              = stmt.column_int64(0),
      .kind            = *kind,
      .slug            = stmt.column_text(2),
      .base_url        = text_opt(stmt, 3),
      .default_project = text_opt(stmt, 4),
      .auth            = *auth,
      .auth_ref        = stmt.column_text(6),
      .created_at      = stmt.column_text(7),
      .updated_at      = stmt.column_text(8),
  };
}

/// @brief Run a single-row SELECT and materialize it.
/// @param conn The connection.
/// @param sql The statement, taking one bind parameter.
/// @param bind Applies the bind; returns whether it succeeded.
/// @return The row, or the failure.
auto select_one(db::connection& conn, std::string_view sql, const std::function<bool(db::statement&)>& bind)
    -> std::expected<external_system, system_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt || !bind(*stmt)) {
    return std::unexpected(system_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(system_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(system_error::not_found);
  }
  return read_row(*stmt);
}

} // namespace

auto system_kind_from_text(std::string_view s) -> std::optional<system_kind> {
  if (s == "jira") {
    return system_kind::jira;
  }
  if (s == "github-issues") {
    return system_kind::github_issues;
  }
  if (s == "gitlab-issues") {
    return system_kind::gitlab_issues;
  }
  if (s == "linear") {
    return system_kind::linear;
  }
  return std::nullopt;
}

auto system_kind_to_text(system_kind k) -> std::string_view {
  switch (k) {
  case system_kind::jira:
    return "jira";
  case system_kind::github_issues:
    return "github-issues";
  case system_kind::gitlab_issues:
    return "gitlab-issues";
  case system_kind::linear:
    return "linear";
  }
  return "jira";
}

auto auth_method_from_text(std::string_view s) -> std::optional<auth_method> {
  if (s == "token-env") {
    return auth_method::token_env;
  }
  if (s == "gh-cli") {
    return auth_method::gh_cli;
  }
  if (s == "oauth-stored") {
    return auth_method::oauth_stored;
  }
  return std::nullopt;
}

auto auth_method_to_text(auth_method m) -> std::string_view {
  switch (m) {
  case auth_method::token_env:
    return "token-env";
  case auth_method::gh_cli:
    return "gh-cli";
  case auth_method::oauth_stored:
    return "oauth-stored";
  }
  return "token-env";
}

auto register_system(db::connection& conn, const register_args& args) -> std::expected<external_system, system_error> {
  auto stmt = conn.prepare("insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) "
                           "values (?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(system_error::query_failed);
  }
  if (!stmt->bind_text(1, system_kind_to_text(args.kind)) || !stmt->bind_text(2, args.slug) ||
      !bind_text_opt(*stmt, 3, args.base_url) || !bind_text_opt(*stmt, 4, args.default_project) ||
      !stmt->bind_text(5, auth_method_to_text(args.auth)) || !stmt->bind_text(6, args.auth_ref)) {
    return std::unexpected(system_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(stepped.error().code_ == k_sqlite_constraint_unique ? system_error::slug_exists
                                                                               : system_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(system_error::query_failed);
  }
  return show_by_id(conn, stmt->column_int64(0));
}

auto register_jira(db::connection& conn, const register_jira_args& args) -> std::expected<external_system, system_error> {
  return register_system(conn, {
                                   .kind            = system_kind::jira,
                                   .slug            = args.slug,
                                   .base_url        = std::string(args.base_url),
                                   .default_project = std::string(args.project),
                                   .auth            = auth_method::token_env,
                                   .auth_ref        = args.auth_env,
                               });
}

auto register_github(db::connection& conn, const register_github_args& args) -> std::expected<external_system, system_error> {
  // The asymmetry documented in system.cppm: an env var selects `token-env`
  // with that variable as the ref; its ABSENCE selects `gh-cli` with the
  // literal ref `default`.
  if (args.auth_env.has_value()) {
    return register_system(conn, {
                                     .kind            = system_kind::github_issues,
                                     .slug            = args.slug,
                                     .base_url        = std::string("https://api.github.com"),
                                     .default_project = std::string(args.project),
                                     .auth            = auth_method::token_env,
                                     .auth_ref        = *args.auth_env,
                                 });
  }
  return register_system(conn, {
                                   .kind            = system_kind::github_issues,
                                   .slug            = args.slug,
                                   .base_url        = std::string("https://api.github.com"),
                                   .default_project = std::string(args.project),
                                   .auth            = auth_method::gh_cli,
                                   .auth_ref        = "default",
                               });
}

auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<external_system, system_error> {
  return select_one(conn, std::format("select {} from external_systems where slug = ?", k_columns),
                    [&](db::statement& stmt) { return stmt.bind_text(1, slug).has_value(); });
}

auto show_by_id(db::connection& conn, std::int64_t id) -> std::expected<external_system, system_error> {
  return select_one(conn, std::format("select {} from external_systems where id = ?", k_columns),
                    [&](db::statement& stmt) { return stmt.bind_int64(1, id).has_value(); });
}

auto list(db::connection& conn) -> std::expected<std::vector<external_system>, system_error> {
  auto stmt = conn.prepare(std::format("select {} from external_systems order by id", k_columns));
  if (!stmt) {
    return std::unexpected(system_error::query_failed);
  }
  std::vector<external_system> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(system_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
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

} // namespace planar::engine::external::system
