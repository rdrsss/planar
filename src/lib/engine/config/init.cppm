/// @file init.cppm
/// @brief `planar.engine.config.init` — bootstrap a Planar database
/// (tech-spec § "engine buckets", plan 996 task 6032). Behavior-preserving
/// port (D2) of `zig/src/engine/init.zig`'s Step 4 (project registration):
/// migration application (Step 1-3 of the Zig header comment) already
/// lives in `planar.db.migrate`'s `apply_all` and is the `cmd/` runtime's
/// job to sequence, exactly as the Zig side documents — this module owns
/// only `register_cwd` (idempotent `INSERT OR IGNORE` / force-repoint) and
/// the `derive_slug` utility, so engine-side callers (tests, this task's
/// own scope) can exercise a full bootstrap without a `cmd/` binary
/// (not yet ported — M4+).
///
/// Deliberately does NOT depend on `planar.engine.identity` — mirrors the
/// Zig oracle, whose `init.zig` imports only `db` and a small LOCAL
/// `project.zig` projection, not the association/membership machinery
/// `identity/project.zig`'s fuller CRUD type would pull in. `registered_project`
/// below is that same minimal projection, independent of
/// `planar.engine.identity.association`'s `project_ref` (which exists for
/// a different caller's needs — see association.cppm's header comment).
module;

export module planar.engine.config.init;

import std;
import planar.db;

namespace planar::engine::config {

/// @brief One `projects` row, as `register_cwd` reads it back. Mirrors
/// zig's local `Project` echo type in init.zig.
export struct registered_project {
  std::int64_t               id;         ///< The row's id.
  std::string                slug;       ///< The project's unique slug.
  std::string                name;       ///< Display name.
  std::optional<std::string> root_path;  ///< Filesystem root path, when registered.
  std::optional<std::string> git_remote; ///< Git remote URL, when captured.
  std::string                created_at; ///< Row creation timestamp.
  std::string                updated_at; ///< Row last-update timestamp.
};

/// @brief Error surface for `register_cwd`.
export enum class init_error : std::uint8_t {
  invalid_path, ///< `cwd` was empty or not absolute.
  query_failed, ///< An underlying SQL statement failed.
};

/// @brief Arguments to `register_cwd`. Mirrors zig's `RegisterCwdArgs`.
export struct register_cwd_args {
  std::string                cwd;        ///< Absolute path of the working directory to register.
  std::optional<std::string> name;       ///< Defaults to `cwd`'s basename.
  std::optional<std::string> slug;       ///< Defaults to `derive_slug(basename(cwd))`.
  std::optional<std::string> git_remote; ///< Optional git remote URL captured by the caller.
  bool force = false; ///< Repoint an existing slug row's `root_path`/name/git_remote under the SAME id (checkout-moved migration
                      ///< path) instead of insert-or-ignore's leave-unchanged default.
};

/// @brief Register `cwd` as a project. Idempotent: a second call from the
/// same cwd (same derived/explicit slug) returns the existing row
/// unchanged unless `force` is set. Mirrors zig's `registerCwd`
/// (`INSERT OR IGNORE` semantics: `name`/`cwd`/`git_remote` are only
/// written on first insert, unless `force` repoints them).
/// @param conn An open, migrated database connection.
/// @param args The cwd (required) plus optional name/slug/git_remote/force.
/// @return The registered (or pre-existing, or repointed) row, or
/// `init_error::invalid_path` if `cwd` is empty or not absolute, or
/// `init_error::query_failed` on a SQL failure.
export auto register_cwd(db::connection& conn, const register_cwd_args& args) -> std::expected<registered_project, init_error>;

/// @brief Derive a project slug from a name (typically the cwd basename).
/// Mirrors zig's `deriveSlug`: lowercase, collapse runs of
/// non-alphanumeric characters to a single '-', trim leading/trailing '-',
/// and fall back to `"project"` when the result would be empty.
/// @param name The input to slugify.
/// @return The derived slug (never empty).
export auto derive_slug(std::string_view name) -> std::string;

} // namespace planar::engine::config
