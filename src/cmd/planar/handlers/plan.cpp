/// @file plan.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.plan`.

module planar.cmd.planar.handlers.plan;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.identity;
import planar.engine.planning;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;

namespace planar::cmd::handlers {

namespace pl = engine::planning;

namespace {

/// @brief The Zig error name for a `plan_error`.
///
/// zig's handler fails with `exit.die(ctx, e, "plan create: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. `SlugConflict` and `SlugNotFound` were captured
/// from the oracle directly (`--slug` collision and `--scope
/// nonexistent-scope`); the rest are transcribed from
/// zig/src/engine/planning/plan.zig's error set, whose members line up
/// one-for-one with this port's `plan_error`.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::plan_error err) -> std::string_view {
  switch (err) {
  case pl::plan_error::not_found:
    return "NotFound";
  case pl::plan_error::slug_conflict:
    return "SlugConflict";
  case pl::plan_error::slug_not_found:
    return "SlugNotFound";
  case pl::plan_error::invalid_parent_cycle:
    return "InvalidParentCycle";
  case pl::plan_error::illegal_transition:
    return "IllegalTransition";
  case pl::plan_error::unknown_status:
    return "UnknownStatus";
  case pl::plan_error::query_failed:
    return "QueryFailed";
  }
  return "Unknown";
}

/// @brief Map a `plan_error` onto this binary's exit-code bucket, per
/// zig/src/cmd/planar/exit.zig's `codeFor`.
///
/// Only `SlugConflict` leaves the generic bucket (`codeFor` maps
/// `error.SlugConflict, error.AlreadyExists => 6`; oracle-confirmed, a
/// duplicate `--slug` exits 6). Everything else — `QueryFailed` on a
/// dangling `--parent`, `SlugNotFound` on an unknown `--scope` — has no
/// arm there and falls to `else => 1`, both oracle-confirmed.
/// @param err The engine error.
/// @return The mapped failure.
auto map_plan_error(pl::plan_error err) -> domain_error {
  auto const kind = err == pl::plan_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("plan create: {}", zig_error_name(err)));
}

} // namespace

auto plan_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation. zig's handler opens with `const
  // d = try runtime.ensureDb();` and only then parses `--status`, so a
  // refused `--status bogus` still leaves a created-and-migrated database
  // behind. Oracle-confirmed against an empty scratch root: exit 1, and a
  // 1179648-byte `planar.db` on disk afterwards. Validating first would be
  // tidier and would silently change when the schema gets applied.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--status` carries a declared default of "draft" (surface.cpp's
  // k_flags_58), but read it defensively: a caller that harvested an
  // absent flag must still get the oracle's default rather than an empty
  // string that would then fail to parse.
  auto const status_raw = cliapp::flag_string(args, "--status").value_or("draft");
  auto const status     = pl::plan_status_from_text(status_raw);
  if (!status) {
    // Exit 1, NOT 2 — zig dies with `error.InvalidStatus`, which has no
    // arm in `codeFor`. Oracle-captured (`--status bogus` -> exit 1).
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", status_raw)));
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};

  auto resolved = resolve_write_scope(ctx, scope_view, "plan create");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // The load-bearing refusal. See this module's header: the alternative
  // is not "a slightly worse message", it is a plan silently filed under
  // `global`.
  if (!scope_flag.has_value() && resolved->reason == engine::identity::derive_reason::project_unassociated) {
    auto const project_slug = resolved->project_slug.value_or("project");
    return std::unexpected(error_from_body(
        domain_error_kind::scope_mismatch,
        std::format("plan create: project has no association; run `planar assoc create project:{} --kind project` then "
                    "`planar assoc add project:{} <repo-path>`, or pass `--scope global` explicitly",
                    project_slug, project_slug)));
  }

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required, so parsing fails first) — but the engine's `title` is a
    // non-optional `std::string`, and defaulting it to "" here would
    // write an untitled row. Refuse instead.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "plan create: title is required"));
  }

  auto created = pl::create_plan(**conn, pl::plan_create_args{
                                             .title          = std::move(*title),
                                             .slug           = cliapp::flag_string(args, "--slug"),
                                             .summary        = cliapp::flag_string(args, "--summary"),
                                             .status         = *status,
                                             .parent_plan_id = cliapp::flag_int(args, "--parent"),
                                             .scope          = resolved->scope,
                                         });
  if (!created) {
    return std::unexpected(map_plan_error(created.error()));
  }

  // Per-renderer terminator contract: `render_text` carries its own
  // trailing newline, `render_json` is a fragment the caller terminates
  // (matching the oracle's `output.emit`, which prints "\n" after
  // stringifying and nothing after `renderText`).
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(*created) << '\n';
  } else {
    ctx.out() << pl::render_text(*created);
  }
  return {};
}

} // namespace planar::cmd::handlers
