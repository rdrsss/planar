/// @file system.cppm
/// @brief `planar.engine.external.system` — the `external_systems` row: a
/// registered operational plane (a Jira site, a GitHub org) (plan 996, task
/// 6041).
///
/// Behavior-preserving port (D2) of all of
/// `zig/src/engine/external/system.zig`. Nothing from that file is deferred.
///
/// This closes one of the deferrals `link.cppm` recorded — "the rest of the
/// bucket's Zig siblings entirely: `sync.zig`, `system.zig`, `agentingest/`"
/// — for `system.zig`. `agentingest/` remains deferred with the
/// `ext propagate` family.
///
/// ## The two register helpers carry CLI defaults, and they are not the same
///
/// `register` is the general form. `register_jira` and `register_github` are
/// the shapes `ext register jira` / `ext register github` pass, and they
/// differ in a way worth stating because it is not symmetric:
///
///   - Jira takes an explicit `base_url` and REQUIRES an auth env var.
///   - GitHub hard-codes `https://api.github.com` as the base URL and treats
///     the auth env var as OPTIONAL: supplying one selects `token-env` auth
///     with that variable as the ref, and omitting it selects `gh-cli` auth
///     with the literal ref `default` — i.e. "shell out to `gh auth token`".
///     That fallback is the whole reason `ext register github --auth-env` is
///     an optional flag while `ext register jira --auth-env` is not.
///
/// ## Slug collision is `slug_exists`, not a generic query failure
///
/// `external_systems.slug` is UNIQUE and the Zig original maps that one
/// constraint to its own error so the CLI can say "already registered"
/// rather than "database error". Detected by SQLite's extended result code
/// (2067), the same way every other bucket in this tree detects it, rather
/// than by matching the driver's message text.
module;

export module planar.engine.external.system;

import std;
import planar.db;

namespace planar::engine::external::system {

/// @brief Which operational plane. Mirrors the `external_systems.kind` CHECK
/// constraint exactly — note the stored values are HYPHENATED for two of the
/// four, which is why the enumerator names and the column values differ.
export enum class system_kind : std::uint8_t {
  jira,          ///< Stored as `jira`.
  github_issues, ///< Stored as `github-issues`.
  gitlab_issues, ///< Stored as `gitlab-issues`.
  linear,        ///< Stored as `linear`.
};

/// @brief Parse a `kind` column value.
/// @param s The stored text.
/// @return The kind, or unset when `s` is not one of the four.
export auto system_kind_from_text(std::string_view s) -> std::optional<system_kind>;

/// @brief The stored text for a kind.
/// @param k The kind.
/// @return The column value.
export auto system_kind_to_text(system_kind k) -> std::string_view;

/// @brief How credentials are obtained. Mirrors the `auth_method` CHECK
/// constraint; all three stored values are hyphenated.
export enum class auth_method : std::uint8_t {
  token_env,    ///< Stored as `token-env`; `auth_ref` names an environment variable.
  gh_cli,       ///< Stored as `gh-cli`; the token comes from `gh auth token`.
  oauth_stored, ///< Stored as `oauth-stored`; not supported by any ported surface.
};

/// @brief Parse an `auth_method` column value.
/// @param s The stored text, hyphenated.
/// @return The method, or unset when unrecognized.
export auto auth_method_from_text(std::string_view s) -> std::optional<auth_method>;

/// @brief The stored, hyphenated text for a method.
/// @param m The method.
/// @return The column value.
export auto auth_method_to_text(auth_method m) -> std::string_view;

/// @brief One `external_systems` row.
export struct external_system {
  std::int64_t               id   = 0;                      ///< The row id.
  system_kind                kind = system_kind::jira;      ///< Which operational plane.
  std::string                slug;                          ///< The operator-facing name; UNIQUE.
  std::optional<std::string> base_url;                      ///< The API root, when the kind needs one.
  std::optional<std::string> default_project;               ///< The default project/repo.
  auth_method                auth = auth_method::token_env; ///< How credentials are obtained.
  std::string                auth_ref;                      ///< The env var name, or `default` for gh-cli.
  std::string                created_at;                    ///< Row creation timestamp.
  std::string                updated_at;                    ///< Row update timestamp.
};

/// @brief The general registration form.
export struct register_args {
  system_kind                kind = system_kind::jira;      ///< Which operational plane.
  std::string_view           slug;                          ///< The operator-facing name.
  std::optional<std::string> base_url;                      ///< The API root.
  std::optional<std::string> default_project;               ///< The default project/repo.
  auth_method                auth = auth_method::token_env; ///< How credentials are obtained.
  std::string_view           auth_ref;                      ///< The env var name, or `default`.
};

/// @brief What `ext register jira` supplies. Every field is required.
export struct register_jira_args {
  std::string_view slug;     ///< The operator-facing name.
  std::string_view base_url; ///< The Jira site root.
  std::string_view project;  ///< The default Jira project key.
  std::string_view auth_env; ///< The environment variable holding the token.
};

/// @brief What `ext register github` supplies.
///
/// `auth_env` is OPTIONAL and that is load-bearing — see this module's
/// header.
export struct register_github_args {
  std::string_view                slug;     ///< The operator-facing name.
  std::string_view                project;  ///< The default `owner/repo`.
  std::optional<std::string_view> auth_env; ///< The token env var; `gh-cli` auth when unset.
};

/// @brief Why a system operation failed. Mirrors the Zig `Error` set.
export enum class system_error : std::uint8_t {
  not_found,    ///< No row with that id or slug.
  slug_exists,  ///< The `slug` UNIQUE was violated.
  query_failed, ///< SQL failure, or an unparseable enum column.
};

/// @brief Insert a system and read it back.
/// @param conn An open, migrated database connection.
/// @param args The row to write.
/// @return The stored row, or the failure.
export auto register_system(db::connection& conn, const register_args& args) -> std::expected<external_system, system_error>;

/// @brief Register a Jira site with the `ext register jira` defaults.
/// @param conn An open, migrated database connection.
/// @param args The CLI's arguments.
/// @return The stored row, or the failure.
export auto register_jira(db::connection& conn, const register_jira_args& args) -> std::expected<external_system, system_error>;

/// @brief Register a GitHub org with the `ext register github` defaults.
///
/// Base URL is always `https://api.github.com`. Auth is `token-env` with the
/// supplied variable, or `gh-cli` with ref `default` when none is supplied.
/// @param conn An open, migrated database connection.
/// @param args The CLI's arguments.
/// @return The stored row, or the failure.
export auto register_github(db::connection& conn, const register_github_args& args)
    -> std::expected<external_system, system_error>;

/// @brief Read one system by slug.
/// @param conn An open, migrated database connection.
/// @param slug The operator-facing name.
/// @return The stored row, or `system_error::not_found`.
export auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<external_system, system_error>;

/// @brief Read one system by id.
/// @param conn An open, migrated database connection.
/// @param id The row id.
/// @return The stored row, or `system_error::not_found`.
export auto show_by_id(db::connection& conn, std::int64_t id) -> std::expected<external_system, system_error>;

/// @brief Every registered system, ordered by id.
/// @param conn An open, migrated database connection.
/// @return The rows, or the failure.
export auto list(db::connection& conn) -> std::expected<std::vector<external_system>, system_error>;

} // namespace planar::engine::external::system
