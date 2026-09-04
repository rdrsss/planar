/// @file scope.cpp
/// @brief Implementation of `planar.cmd.planar_ext.scope`.

module planar.cmd.planar_ext.scope;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.identity;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {

namespace {

/// @brief The Zig error name a given `scope_error` corresponds to.
///
/// zig/src/cmd/planar/handlers/annotate/add.zig fails with
/// `exit.die(ctx, e, "annotate add: resolving scope failed: {s}",
/// .{@errorName(e)})` — the message interpolates Zig's own CamelCase error
/// NAME, not a prose description. Reproducing that shape is what keeps a
/// script matching on the message working.
///
/// Honest limit: none of these four are reachable through the verb subset
/// this task ports, so none of the four strings could be captured from the
/// oracle. They are derived from `@errorName` on the corresponding Zig
/// error tag rather than observed, and this comment exists so the next
/// reader does not mistake them for oracle-pinned bytes. The FORMAT around
/// them is pinned — `annotate add`'s missing-`--anchor-path` refusal on
/// the same handler was captured (`error: --anchor-path is required`, exit
/// 2), which fixes the prefix, the terminator and the stream.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(engine::identity::scope_error err) -> std::string_view {
  switch (err) {
  case engine::identity::scope_error::query_failed:
    return "QueryFailed";
  case engine::identity::scope_error::invalid_path:
    return "InvalidPath";
  case engine::identity::scope_error::slug_not_found:
    return "SlugNotFound";
  case engine::identity::scope_error::scope_mismatch:
    return "ScopeMismatch";
  }
  return "Unknown";
}

} // namespace

auto map_scope_error(engine::identity::scope_error err, std::string_view verb) -> domain_error {
  auto const kind = [err] {
    switch (err) {
    case engine::identity::scope_error::scope_mismatch:
      return domain_error_kind::scope_mismatch;
    case engine::identity::scope_error::invalid_path:
      return domain_error_kind::invalid_input;
    case engine::identity::scope_error::slug_not_found:
    case engine::identity::scope_error::query_failed:
      return domain_error_kind::generic_failure;
    }
    return domain_error_kind::generic_failure;
  }();
  return error_from_body(kind, std::format("{}: resolving scope failed: {}", verb, zig_error_name(err)));
}

auto map_scope_failure(const engine::identity::write_scope_failure& failure, std::string_view cwd, std::string_view verb)
    -> domain_error {
  if (failure.ambiguity.has_value()) {
    // The meta-workspace refusal (task 6134). Its message NAMES the two
    // `--scope` values the operator may choose between, so it does not go
    // through `map_scope_error`'s generic "resolving scope failed: <Name>"
    // shape at all. Wording is verbatim from the oracle
    // (zig/src/cmd/planar/scope.zig:108-114), which is why the two choices
    // have to travel out of the engine rather than be recomputed here.
    return error_from_body(domain_error_kind::scope_mismatch,
                           std::format("ambiguous meta workspace root {}: choose `--scope {}` for cross-repo/meta-level "
                                       "work or `--scope {}` for root-repo work",
                                       cwd, failure.ambiguity->assoc_scope, failure.ambiguity->repo_scope));
  }
  return map_scope_error(failure.code, verb);
}

auto guard_with_membership(db::connection& conn, std::optional<std::string_view> entity_scope,
                           std::optional<std::string_view> write_scope) -> bool {
  if (engine::identity::check_scope_guard(entity_scope, write_scope)) {
    return true;
  }
  // Only a genuine mismatch between two PRESENT scopes is ever widened.
  if (!entity_scope.has_value() || !write_scope.has_value()) {
    return false;
  }
  constexpr std::string_view k_repo_prefix{"repo:"};
  constexpr std::string_view k_assoc_prefix{"assoc:"};
  if (!entity_scope->starts_with(k_repo_prefix)) {
    return false;
  }
  auto assoc_slug = *write_scope;
  if (assoc_slug.starts_with(k_assoc_prefix)) {
    assoc_slug.remove_prefix(k_assoc_prefix.size());
  }
  // A repo write scope never widens to reach anything; the asymmetry is the
  // point of the rule.
  if (assoc_slug.starts_with(k_repo_prefix)) {
    return false;
  }
  auto const project_slug = entity_scope->substr(k_repo_prefix.size());

  auto statement = conn.prepare("select 1 from project_associations pa "
                                "join projects p on p.id = pa.project_id "
                                "join associations a on a.id = pa.association_id "
                                "where p.slug = ? and a.slug = ? limit 1");
  if (!statement) {
    return false;
  }
  if (!statement->bind_text(1, project_slug) || !statement->bind_text(2, assoc_slug)) {
    return false;
  }
  auto const stepped = statement->step();
  return stepped.has_value() && *stepped == db::step_result::row;
}

auto resolve_write_scope(context& ctx, std::optional<std::string_view> scope_flag, std::string_view verb)
    -> std::expected<engine::identity::write_scope_resolution, domain_error> {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const cwd      = ctx.cwd().string();
  auto       resolved = engine::identity::resolve_for_write(**conn, scope_flag, cwd);
  if (!resolved) {
    return std::unexpected(map_scope_failure(resolved.error(), cwd, verb));
  }
  return *resolved;
}

auto resolve_read_scope_slugs(context& ctx) -> std::expected<std::vector<std::string>, domain_error> {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const cwd = ctx.cwd().string();
  auto       set = engine::identity::resolve_read_scope_set(**conn, cwd, std::nullopt);
  if (!set) {
    return std::unexpected(map_scope_error(set.error(), "resolving read scope"));
  }
  if (set->empty()) {
    // Verbatim from the oracle (zig/src/cmd/planar/scope.zig's callers in
    // plan/list.zig and task/list.zig, which share this exact string).
    // `error.NoReadScope` has no arm in `codeFor`, so it lands in the
    // generic bucket at exit 1 — oracle-confirmed by running `plan list`
    // and `task list` from a directory outside every registered scope.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        "cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global"));
  }
  auto slugs = engine::identity::read_scope_filter_slugs(**conn, *set);
  if (!slugs) {
    return std::unexpected(map_scope_error(slugs.error(), "resolving read scope"));
  }
  return *slugs;
}

} // namespace planar::cmd
