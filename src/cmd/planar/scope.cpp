/// @file scope.cpp
/// @brief Implementation of `planar.cmd.planar.scope`.

module planar.cmd.planar.scope;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

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

auto resolve_write_scope(context& ctx, std::optional<std::string_view> scope_flag, std::string_view verb)
    -> std::expected<engine::identity::write_scope_resolution, domain_error> {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto resolved = engine::identity::resolve_for_write(**conn, scope_flag, ctx.cwd().string());
  if (!resolved) {
    return std::unexpected(map_scope_error(resolved.error(), verb));
  }
  return *resolved;
}

} // namespace planar::cmd
