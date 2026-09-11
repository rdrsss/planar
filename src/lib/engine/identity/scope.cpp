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

auto resolve_meta_workspace_write_scope(db::connection& conn, std::string_view cwd)
    -> std::expected<meta_write_resolution, scope_error> {
  // Query 1: is `cwd` EXACTLY both the org root and a member project root?
  // Then it names two scopes equally well and there is no safe default.
  {
    auto stmt = conn.prepare("select a.slug, p.slug "
                             "from associations a "
                             "join project_associations pa on pa.association_id = a.id "
                             "join projects p on p.id = pa.project_id "
                             "where a.kind = 'org' "
                             "  and p.root_path = ? "
                             "  and json_extract(a.config_json, '$.root_path') = ? "
                             "  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo' "
                             "order by a.id "
                             "limit 1");
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (!stmt->bind_text(1, cwd) || !stmt->bind_text(2, cwd)) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step == db::step_result::row) {
      return meta_write_resolution{
          .which   = meta_write_resolution::arm::ambiguous,
          .choices = meta_ambiguity{.assoc_scope = std::format("assoc:{}", stmt->column_text(0)),
                                    .repo_scope  = std::format("repo:{}", stmt->column_text(1))},
      };
    }
  }

  // Query 2: the longest member project root `cwd` sits under, where `cwd`
  // is also under the org root. `order by length(p.root_path) desc` makes
  // the first match the most specific one, so nested member repositories
  // resolve to the inner one.
  auto stmt = conn.prepare("select p.slug, p.root_path, json_extract(a.config_json, '$.root_path') "
                           "from projects p "
                           "join project_associations pa on pa.project_id = p.id "
                           "join associations a on a.id = pa.association_id "
                           "where a.kind = 'org' "
                           "  and p.root_path is not null "
                           "  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo' "
                           "  and json_extract(a.config_json, '$.root_path') is not null "
                           "order by length(p.root_path) desc, p.id");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return meta_write_resolution{.which = meta_write_resolution::arm::none};
    }
    auto const slug         = stmt->column_text(0);
    auto const project_root = stmt->column_text(1);
    auto const org_root     = stmt->column_text(2);
    if (path_has_prefix(cwd, org_root) && path_has_prefix(cwd, project_root)) {
      return meta_write_resolution{.which = meta_write_resolution::arm::repo_scope, .repo_scope = std::format("repo:{}", slug)};
    }
  }
}

// SPIKE (task 6746): defined below, next to the read resolver whose ranking
// it shares. Forward-declared because `resolve_for_write` sits above
// `derive_candidates` in this file.
auto derive_write_scope_ranked(db::connection& conn, std::string_view cwd) -> std::expected<scope_resolution, scope_error>;

auto resolve_for_write(db::connection& conn, std::optional<std::string_view> scope_flag, std::string_view cwd)
    -> std::expected<write_scope_resolution, write_scope_failure> {
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

  // The meta-workspace arm (task 6134), which runs ONLY when no explicit
  // `--scope` was passed — the flag above is the operator's stated intent
  // and settles the ambiguity by itself, which is why the oracle returns
  // before ever probing.
  auto meta = resolve_meta_workspace_write_scope(conn, cwd);
  if (!meta) {
    return std::unexpected(write_scope_failure{.code = meta.error()});
  }
  switch (meta->which) {
  case meta_write_resolution::arm::ambiguous:
    // Refuse rather than pick. Standing exactly on a meta root, `cwd` names
    // the org and the root repo equally well, and silently resolving to
    // either is the silent-wrong-answer class this port has been closing.
    return std::unexpected(write_scope_failure{.code = scope_error::scope_mismatch, .ambiguity = meta->choices});
  case meta_write_resolution::arm::repo_scope:
    // Writes inside a meta workspace land on the concrete member repo.
    // `reason` is `project_single_association` to match zig, which reports
    // the same reason for this arm.
    return write_scope_resolution{.scope              = meta->repo_scope,
                                  .from_explicit_flag = false,
                                  .reason             = derive_reason::project_single_association,
                                  .project_slug       = std::nullopt};
  case meta_write_resolution::arm::none:
    break;
  }

  // SPIKE (task 6746): was `derive_from_cwd`, which maps the cwd's project to
  // its single association and therefore NEVER yields a `repo:` scope --
  // measured divergence from `scope show`, which applies the specificity
  // ranking and does. This routes the write path through the same ranking.
  auto derived = derive_write_scope_ranked(conn, cwd);
  if (!derived) {
    return std::unexpected(write_scope_failure{.code = derived.error()});
  }
  return write_scope_resolution{
      .scope = derived->scope, .from_explicit_flag = false, .reason = derived->reason, .project_slug = derived->project_slug};
}

auto reason_from_source(std::string_view source) -> std::string_view {
  if (source == "user") {
    return "explicit member";
  }
  if (source == "auto:git-remote") {
    return "from git remote";
  }
  if (source == "auto:path") {
    return "from parent directory";
  }
  if (source == "auto:lang") {
    return "from language ecosystem";
  }
  return source;
}

auto suggest(db::connection& conn, std::string_view root_path) -> std::expected<std::vector<scope_suggestion>, scope_error> {
  std::int64_t project_id = 0;
  {
    auto stmt = conn.prepare("select id from projects where root_path = ?");
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, root_path); !bound) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      // Not a project root. An empty answer, not a failure — see the doc
      // comment.
      return std::vector<scope_suggestion>{};
    }
    project_id = stmt->column_int64(0);
  }

  auto stmt = conn.prepare("select a.id, a.slug, pa.source "
                           "from project_associations pa "
                           "join associations a on a.id = pa.association_id "
                           "where pa.project_id = ? "
                           "order by a.slug");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, project_id); !bound) {
    return std::unexpected(scope_error::query_failed);
  }

  std::vector<scope_suggestion> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    auto const source = stmt->column_text(2);
    out.push_back(scope_suggestion{.slug           = stmt->column_text(1),
                                   .association_id = stmt->column_int64(0),
                                   .reason         = std::string{reason_from_source(source)}});
  }
  return out;
}

// =========================================================================
// The READ set (task 6141) — port of zig/src/cmd/planar/scope.zig's
// `resolveForReadSet` / `readScopeFilterSlugs` and their private helpers.
// =========================================================================

namespace {

/// @brief One cwd-prefix match, before the specificity contest picks a
/// winner. Mirrors zig's `Candidate`.
struct candidate {
  std::int64_t id;       ///< The project's or association's row id.
  std::string  kind;     ///< `"repo"` for a project, else the association's `kind` column.
  std::size_t  root_len; ///< Length of the root path that matched, the tie-breaker.
};

/// @brief How specific a candidate kind is; LOWER wins. Mirrors zig's
/// `specificityRank` exactly, including the unknown-kind bucket.
/// @param kind The candidate's kind token.
/// @return The rank, 1 (most specific) through 6 (unrecognized).
auto specificity_rank(std::string_view kind) -> std::uint8_t {
  if (kind == "project") {
    return 1;
  }
  if (kind == "ad-hoc" || kind == "personal") {
    return 2;
  }
  if (kind == "client") {
    return 3;
  }
  if (kind == "repo") {
    return 4;
  }
  if (kind == "org") {
    return 5;
  }
  return 6;
}

/// @brief Whether `items` already holds an identical candidate. Mirrors
/// zig's `candidateExists` — identity is (id, root_len, kind), all three.
/// @param items The candidates collected so far.
/// @param c The candidate to test.
/// @return True when an identical entry is already present.
auto candidate_exists(const std::vector<candidate>& items, const candidate& c) -> bool {
  return std::ranges::any_of(
      items, [&c](const candidate& item) { return item.id == c.id && item.root_len == c.root_len && item.kind == c.kind; });
}

/// @brief Expand an `org` association into the full workspace read set.
///
/// Order is load-bearing and mirrors zig's `expandWorkspaceReadSet`: the
/// org association first, then every OTHER `project`-kind association
/// sharing a member project with it (by id), then every member project as a
/// repo scope (by id). Callers turn this into an OR-ed SQL disjunction, so
/// the order is not semantically required — but it is what the oracle's
/// `--json` scope lists render, and reproducing it costs nothing.
/// @param conn An open, migrated database connection.
/// @param org_assoc_id The org association's row id.
/// @return The expanded set, or `scope_error::query_failed`.
auto expand_workspace_read_set(db::connection& conn, std::int64_t org_assoc_id)
    -> std::expected<std::vector<read_scope>, scope_error> {
  std::vector<read_scope> out;
  out.push_back(read_scope{.kind = scope_kind::association, .id = org_assoc_id});

  std::vector<std::int64_t> project_ids;
  {
    auto stmt = conn.prepare("select project_id from project_associations where association_id = ? order by project_id");
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, org_assoc_id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    for (;;) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(scope_error::query_failed);
      }
      if (*step != db::step_result::row) {
        break;
      }
      project_ids.push_back(stmt->column_int64(0));
    }
  }

  {
    auto stmt = conn.prepare("select distinct a.id from associations a "
                             "join project_associations pa on pa.association_id = a.id "
                             "where a.kind = 'project' and a.id != ? "
                             "  and pa.project_id in (select project_id from project_associations where association_id = ?) "
                             "order by a.id");
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, org_assoc_id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, org_assoc_id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    for (;;) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(scope_error::query_failed);
      }
      if (*step != db::step_result::row) {
        break;
      }
      out.push_back(read_scope{.kind = scope_kind::association, .id = stmt->column_int64(0)});
    }
  }

  for (auto const pid : project_ids) {
    out.push_back(read_scope{.kind = scope_kind::repo, .id = pid});
  }
  return out;
}

/// @brief The meta-workspace arm of the read set, run before the generic
/// candidate contest. Mirrors zig's `resolveMetaWorkspaceReadSet`.
///
/// Standing exactly AT an org root expands to the whole workspace (a read
/// wants everything visible from there); standing deeper inside a member
/// project narrows to that repo. This is where read and write diverge most
/// sharply — the same cwd that makes `resolve_for_write` REFUSE as ambiguous
/// makes a read expansive.
/// @param conn An open, migrated database connection.
/// @param cwd The absolute working-directory path.
/// @return The resolved set, or `std::nullopt` when `cwd` is not inside any
/// registered meta workspace, or `scope_error::query_failed`.
auto resolve_meta_workspace_read_set(db::connection& conn, std::string_view cwd)
    -> std::expected<std::optional<std::vector<read_scope>>, scope_error> {
  auto stmt = conn.prepare("select p.id, p.root_path, json_extract(a.config_json, '$.root_path'), a.id "
                           "from projects p "
                           "join project_associations pa on pa.project_id = p.id "
                           "join associations a on a.id = pa.association_id "
                           "where a.kind = 'org' "
                           "  and p.root_path is not null "
                           "  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo' "
                           "  and json_extract(a.config_json, '$.root_path') is not null "
                           "order by length(p.root_path) desc, p.id");
  if (!stmt) {
    return std::unexpected(scope_error::query_failed);
  }
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::optional<std::vector<read_scope>>{};
    }
    auto const project_id   = stmt->column_int64(0);
    auto const project_root = stmt->column_text(1);
    auto const org_root     = stmt->column_text(2);
    auto const org_assoc_id = stmt->column_int64(3);
    if (!path_has_prefix(cwd, org_root) || !path_has_prefix(cwd, project_root)) {
      continue;
    }
    if (cwd == org_root) {
      auto expanded = expand_workspace_read_set(conn, org_assoc_id);
      if (!expanded) {
        return std::unexpected(expanded.error());
      }
      return std::optional<std::vector<read_scope>>{std::move(*expanded)};
    }
    return std::optional<std::vector<read_scope>>{
        std::vector<read_scope>{read_scope{.kind = scope_kind::repo, .id = project_id}}};
  }
}

/// @brief Collect every project / association / org whose registered root is
/// a path prefix of `cwd`. Mirrors zig's `deriveCandidates`, all three
/// queries and their order.
///
/// The third query reads the org root out of `config_json` with SQL
/// `json_extract` rather than parsing JSON in C++ — the same way
/// `resolve_meta_workspace_write_scope` two functions up reads the same
/// field. The Zig original hand-parses because its SQL layer surfaces the
/// column as text; the extracted value is identical.
/// @param conn An open, migrated database connection.
/// @param cwd The absolute working-directory path.
/// @return The candidates, in query order, or `scope_error::query_failed`.
auto derive_candidates(db::connection& conn, std::string_view cwd) -> std::expected<std::vector<candidate>, scope_error> {
  std::vector<candidate> out;

  auto const collect = [&](std::string_view sql, bool kind_is_literal_repo) -> std::expected<void, scope_error> {
    auto stmt = conn.prepare(sql);
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    for (;;) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(scope_error::query_failed);
      }
      if (*step != db::step_result::row) {
        return {};
      }
      auto const id   = stmt->column_int64(0);
      auto const kind = kind_is_literal_repo ? std::string{"repo"} : stmt->column_text(1);
      auto const root = kind_is_literal_repo ? stmt->column_text(1) : stmt->column_text(2);
      if (!path_has_prefix(cwd, root)) {
        continue;
      }
      candidate c{.id = id, .kind = kind, .root_len = root.size()};
      if (!candidate_exists(out, c)) {
        out.push_back(std::move(c));
      }
    }
  };

  if (auto r = collect("select id, root_path from projects where root_path is not null order by root_path", true); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = collect("select a.id, a.kind, p.root_path from associations a "
                       "join project_associations pa on pa.association_id = a.id "
                       "join projects p on p.id = pa.project_id "
                       "where p.root_path is not null order by a.id, p.root_path",
                       false);
      !r) {
    return std::unexpected(r.error());
  }
  if (auto r = collect("select id, kind, json_extract(config_json, '$.root_path') from associations "
                       "where kind = 'org' and config_json is not null and config_json != '' "
                       "  and json_extract(config_json, '$.root_path') is not null "
                       "order by id",
                       false);
      !r) {
    return std::unexpected(r.error());
  }
  return out;
}

} // namespace

auto resolve_read_scope_set(db::connection& conn, std::string_view cwd, std::optional<std::string_view> override)
    -> std::expected<std::vector<read_scope>, scope_error> {
  if (override.has_value()) {
    // VALIDATED, unlike the write path's verbatim pass-through. See this
    // function's doc comment for why the asymmetry is the oracle's.
    auto ref = resolve_slug(conn, *override);
    if (!ref) {
      return std::unexpected(ref.error());
    }
    return std::vector<read_scope>{read_scope{.kind = ref->kind, .id = ref->id.value_or(0)}};
  }

  auto meta = resolve_meta_workspace_read_set(conn, cwd);
  if (!meta) {
    return std::unexpected(meta.error());
  }
  if (meta->has_value()) {
    return std::move(**meta);
  }

  auto candidates = derive_candidates(conn, cwd);
  if (!candidates) {
    return std::unexpected(candidates.error());
  }
  if (candidates->empty()) {
    return std::vector<read_scope>{};
  }

  // Two passes, exactly as zig does it. The first finds the best (rank,
  // root_len) pair; the second counts how many candidates hit it. A TIE is
  // not resolved by picking one — it returns the empty set, which the caller
  // turns into a refusal. Collapsing a tie to `candidates[0]` would produce
  // a listing scoped to an arbitrary one of two equally-good scopes.
  auto best_rank     = specificity_rank((*candidates)[0].kind);
  auto best_root_len = (*candidates)[0].root_len;
  for (auto const& c : std::span{*candidates}.subspan(1)) {
    auto const r = specificity_rank(c.kind);
    if (r < best_rank) {
      best_rank     = r;
      best_root_len = c.root_len;
    } else if (r == best_rank && c.root_len > best_root_len) {
      best_root_len = c.root_len;
    }
  }
  std::size_t top_count = 0;
  candidate   winner    = (*candidates)[0];
  for (auto const& c : *candidates) {
    if (specificity_rank(c.kind) == best_rank && c.root_len == best_root_len) {
      ++top_count;
      winner = c;
    }
  }
  if (top_count != 1) {
    return std::vector<read_scope>{};
  }

  if (winner.kind == "repo") {
    return std::vector<read_scope>{read_scope{.kind = scope_kind::repo, .id = winner.id}};
  }
  if (winner.kind == "org") {
    return expand_workspace_read_set(conn, winner.id);
  }
  return std::vector<read_scope>{read_scope{.kind = scope_kind::association, .id = winner.id}};
}

// SPIKE (task 6746): the write-path twin of `resolve_read_scope_set`'s
// winner selection. Same candidates, same specificity ranking, same
// longest-root tie-breaker -- but it yields ONE scope label rather than a
// read set, and it does NOT expand an org winner into its members (a write
// lands in one scope; a read spans several).
//
// Deliberately conservative for the spike: an empty candidate set and a TIE
// both fall back to the pre-spike answer (unset scope => global) rather than
// refusing, so this measures exactly one change -- "a cwd-derived write picks
// the most specific scope, as `scope show` already reports" -- and not the
// separate question of whether those two cases should start refusing.
auto derive_write_scope_ranked(db::connection& conn, std::string_view cwd) -> std::expected<scope_resolution, scope_error> {
  if (cwd.empty() || cwd.front() != '/') {
    return std::unexpected(scope_error::invalid_path);
  }

  auto candidates = derive_candidates(conn, cwd);
  if (!candidates) {
    return std::unexpected(candidates.error());
  }
  if (candidates->empty()) {
    return scope_resolution{.scope = std::nullopt, .reason = derive_reason::no_project_match, .project_slug = std::nullopt};
  }

  auto best_rank     = specificity_rank((*candidates)[0].kind);
  auto best_root_len = (*candidates)[0].root_len;
  for (auto const& c : std::span{*candidates}.subspan(1)) {
    auto const r = specificity_rank(c.kind);
    if (r < best_rank) {
      best_rank     = r;
      best_root_len = c.root_len;
    } else if (r == best_rank && c.root_len > best_root_len) {
      best_root_len = c.root_len;
    }
  }
  std::size_t top_count = 0;
  candidate   winner    = (*candidates)[0];
  for (auto const& c : *candidates) {
    if (specificity_rank(c.kind) == best_rank && c.root_len == best_root_len) {
      ++top_count;
      winner = c;
    }
  }
  if (top_count != 1) {
    return scope_resolution{
        .scope = std::nullopt, .reason = derive_reason::project_multiple_associations, .project_slug = std::nullopt};
  }

  auto const  table = winner.kind == "repo" ? "projects" : "associations";
  std::string slug;
  {
    auto stmt = conn.prepare(std::format("select coalesce(slug,'') from {} where id = ?", table));
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, winner.id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(scope_error::slug_not_found);
    }
    slug = stmt->column_text(0);
  }
  if (slug.empty()) {
    return std::unexpected(scope_error::slug_not_found);
  }

  if (winner.kind == "repo") {
    // PRESERVE the unassociated arm verbatim. A registered project with no
    // association is a DELIBERATE, separately-tested behaviour: `plan create`
    // keys an exit-5 refusal on this exact `reason`, and `task add` /
    // `scenario add` deliberately do not. Emitting a `repo:` scope here would
    // silently retire that refusal -- which is what the first spike run did,
    // and it is orthogonal to the question this spike is measuring.
    auto assoc_count = conn.prepare("select count(*) from project_associations where project_id = ?");
    if (!assoc_count) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = assoc_count->bind_int64(1, winner.id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = assoc_count->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step == db::step_result::row && assoc_count->column_int64(0) == 0) {
      return scope_resolution{
          .scope = std::nullopt, .reason = derive_reason::project_unassociated, .project_slug = slug};
    }
    return scope_resolution{.scope        = std::format("repo:{}", slug),
                            .reason       = derive_reason::project_single_association,
                            .project_slug = slug};
  }
  // An association winner keeps the BARE slug the pre-spike path returned --
  // `resolve_slug` parses a bare slug as an association, so this stays
  // byte-compatible with what every guarded comparison already sees.
  return scope_resolution{
      .scope = slug, .reason = derive_reason::project_single_association, .project_slug = std::nullopt};
}

auto read_scope_filter_slugs(db::connection& conn, std::span<const read_scope> scopes)
    -> std::expected<std::vector<std::string>, scope_error> {
  std::vector<std::string> out;
  out.reserve(scopes.size());
  for (auto const& row : scopes) {
    if (row.kind == scope_kind::global) {
      out.emplace_back("global");
      continue;
    }
    auto const table  = row.kind == scope_kind::association ? "associations" : "projects";
    auto const prefix = row.kind == scope_kind::association ? "assoc:" : "repo:";
    auto       stmt   = conn.prepare(std::format("select coalesce(slug,'') from {} where id = ?", table));
    if (!stmt) {
      return std::unexpected(scope_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, row.id); !b) {
      return std::unexpected(scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(scope_error::slug_not_found);
    }
    out.push_back(std::format("{}{}", prefix, stmt->column_text(0)));
  }
  return out;
}

} // namespace planar::engine::identity
