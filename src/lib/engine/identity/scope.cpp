/// @file scope.cpp
/// @brief Implementation of `planar.engine.identity.scope` (see scope.cppm).

module;

module planar.engine.identity.scope;

import std;
import planar.db;
import planar.scope_ref;

namespace planar::engine::identity {

// Alias, not a bare `import planar.scope_ref` name: this module's own
// exported struct is ALSO named `scope_ref` (see scope.cppm), so the
// unqualified name `scope_ref` inside this namespace resolves to that
// local struct, not the imported `planar::scope_ref` namespace. `sref`
// sidesteps the collision without renaming either public API.
namespace sref = planar::scope_ref;

namespace {

/// @brief True when `target` equals `root` or starts with `root/`. Both
/// arguments must be clean paths (no trailing slash except the filesystem
/// root). Mirrors zig's `pathHasPrefix`.
auto path_has_prefix(std::string_view target, std::string_view root) -> bool {
  if (target == root) {
    return true;
  }
  if (!target.starts_with(root)) {
    return false;
  }
  if (target.size() <= root.size()) {
    return false;
  }
  return target[root.size()] == '/';
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` (extracted, plan
/// 996 task 6089, D19) onto this module's own `scope_kind`. A 1:1 mapping
/// kept as an explicit `switch` (rather than sharing the enum type
/// outright) so this module's public API surface is unaffected by the
/// extraction.
auto to_local_kind(sref::scope_kind k) -> scope_kind {
  switch (k) {
  case sref::scope_kind::global:
    return scope_kind::global;
  case sref::scope_kind::association:
    return scope_kind::association;
  case sref::scope_kind::repo:
    return scope_kind::repo;
  }
  return scope_kind::global; // unreachable
}

auto to_local_error(sref::error e) -> scope_error {
  switch (e) {
  case sref::error::query_failed:
    return scope_error::query_failed;
  case sref::error::slug_not_found:
    return scope_error::slug_not_found;
  }
  return scope_error::query_failed; // unreachable
}

struct project_match {
  std::int64_t id;
  std::string  slug;
};

/// @brief Find the project whose `root_path` is the longest prefix of
/// `path`. Mirrors zig's `lookupProjectByPrefix`.
auto lookup_project_by_prefix(db::connection& conn, std::string_view path)
    -> std::expected<std::optional<project_match>, scope_error> {
  auto stmt =
      conn.prepare("select id, slug, root_path from projects where root_path is not null order by length(root_path) desc");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }

  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step == db::step_result::done) {
      return std::optional<project_match>{};
    }
    if (stmt->is_null(2)) {
      continue;
    }
    const auto root = stmt->column_text(2);
    if (path_has_prefix(path, root)) {
      return project_match{.id = stmt->column_int64(0), .slug = stmt->column_text(1)};
    }
  }
}

} // namespace

auto derive_from_cwd(db::connection& conn, std::string_view cwd) -> std::expected<scope_resolution, scope_error> {
  if (cwd.empty() || cwd.front() != '/') {
    return std::unexpected(scope_error::invalid_path);
  }

  auto match = lookup_project_by_prefix(conn, cwd);
  if (!match) {
    return std::unexpected(match.error());
  }

  if (!match->has_value()) {
    return scope_resolution{.scope = std::nullopt, .reason = derive_reason::no_project_match, .project_slug = std::nullopt};
  }

  const auto& pm = **match;

  auto stmt = conn.prepare("select a.slug from associations a "
                           "join project_associations pa on pa.association_id = a.id "
                           "where pa.project_id = ? order by a.id");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, pm.id); !bound) {
    return std::unexpected(scope_error::query_failed);
  }

  std::vector<std::string> assoc_slugs;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    assoc_slugs.push_back(stmt->column_text(0));
  }

  if (assoc_slugs.empty()) {
    return scope_resolution{.scope = std::nullopt, .reason = derive_reason::project_unassociated, .project_slug = pm.slug};
  }
  if (assoc_slugs.size() == 1) {
    return scope_resolution{
        .scope = assoc_slugs.front(), .reason = derive_reason::project_single_association, .project_slug = pm.slug};
  }
  return scope_resolution{.scope = std::nullopt, .reason = derive_reason::project_multiple_associations, .project_slug = pm.slug};
}

auto resolve_slug(db::connection& conn, std::string_view slug) -> std::expected<scope_ref, scope_error> {
  // Delegates to `planar.scope_ref` (plan 996 task 6089, D19) — the
  // `global` / `repo:<slug>` / `assoc:<slug>` / bare-association grammar
  // and its DB lookup now live in exactly one place, shared with
  // `engine_planning`'s plan.cpp/task.cpp.
  auto resolved = sref::resolve(conn, slug);
  if (!resolved) {
    return std::unexpected(to_local_error(resolved.error()));
  }
  return scope_ref{.kind = to_local_kind(resolved->kind), .id = resolved->id};
}

auto slug_from_ref(db::connection& conn, scope_kind kind, std::optional<std::int64_t> id)
    -> std::expected<std::optional<std::string>, scope_error> {
  if (kind == scope_kind::global) {
    return std::optional<std::string>{};
  }
  if (!id.has_value()) {
    return std::unexpected(scope_error::slug_not_found);
  }

  if (kind == scope_kind::association) {
    auto stmt = conn.prepare("select slug from associations where id = ?");
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, *id); !bound) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(scope_error::slug_not_found);
    }
    return std::optional<std::string>{stmt->column_text(0)};
  }

  // scope_kind::repo
  auto stmt = conn.prepare("select slug from projects where id = ?");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, *id); !bound) {
    return std::unexpected(scope_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(scope_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(scope_error::slug_not_found);
  }
  return std::optional<std::string>{std::format("repo:{}", stmt->column_text(0))};
}

auto check_scope_guard(std::optional<std::string_view> entity_scope, std::optional<std::string_view> write_scope)
    -> std::expected<void, scope_error> {
  if (!entity_scope.has_value()) {
    return {};
  }
  if (!write_scope.has_value()) {
    return std::unexpected(scope_error::scope_mismatch);
  }
  if (sref::normalize_assoc(*entity_scope) != sref::normalize_assoc(*write_scope)) {
    return std::unexpected(scope_error::scope_mismatch);
  }
  return {};
}

auto guard_write(std::optional<std::string_view> entity_scope, std::optional<std::string_view> write_scope, bool no_scope_check)
    -> std::expected<void, scope_error> {
  if (no_scope_check) {
    return {};
  }
  return check_scope_guard(entity_scope, write_scope);
}

auto resolve_for_write(db::connection& conn, std::optional<std::string_view> scope_flag, std::string_view cwd)
    -> std::expected<write_scope_resolution, scope_error> {
  if (scope_flag.has_value()) {
    // Threaded through VERBATIM — no DB lookup, no validation. Mirrors
    // zig's resolveForWrite (zig/src/cmd/planar/scope.zig:84-93), which
    // hands the override straight through without calling resolveSlug.
    // This includes the literal string "global": zig carries it as-is
    // rather than collapsing it to an unset scope, so a guarded write
    // against a global-scoped entity (unset entity_scope) still succeeds
    // unconditionally per check_scope_guard's first branch, and a
    // guarded write against a NON-global entity correctly refuses
    // (write_scope "global" != the entity's scope label).
    return write_scope_resolution{
        .scope = std::string(*scope_flag), .from_explicit_flag = true, .reason = derive_reason::no_project_match};
  }

  auto derived = derive_from_cwd(conn, cwd);
  if (!derived) {
    return std::unexpected(derived.error());
  }
  return write_scope_resolution{
      .scope = derived->scope, .from_explicit_flag = false, .reason = derived->reason, .project_slug = derived->project_slug};
}

} // namespace planar::engine::identity
