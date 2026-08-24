/// @file task.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.task`.

module;

#include <unistd.h>

module planar.cmd.planar.handlers.task;

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

/// @brief The Zig error name for a `task_error`.
///
/// The oracle fails with `exit.die(ctx, e, "task add: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG rather than prose. `SlugConflict` (`--slug`
/// colliding with an EXISTING task — note `tasks.slug` is globally
/// unique, so the collision does not have to be in the same plan) and
/// `SlugNotFound` (`--scope nosuchscope`) were captured from the oracle
/// directly; the rest are transcribed from
/// zig/src/engine/planning/task.zig's error set, whose members line up
/// one-for-one with this port's `task_error`.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::task_error err) -> std::string_view {
  switch (err) {
  case pl::task_error::not_found:
    return "NotFound";
  case pl::task_error::slug_not_found:
    return "SlugNotFound";
  case pl::task_error::slug_conflict:
    return "SlugConflict";
  case pl::task_error::illegal_transition:
    return "IllegalTransition";
  case pl::task_error::unknown_status:
    return "UnknownStatus";
  case pl::task_error::invalid_due_at:
    // Oracle-captured: `task add "x" --due not-a-date` prints
    // `error: task add: InvalidDueAt` and exits 1.
    return "InvalidDueAt";
  case pl::task_error::query_failed:
    return "QueryFailed";
  }
  return "Unknown";
}

/// @brief Map a `task_error` onto this binary's exit-code bucket, per
/// zig/src/cmd/planar/exit.zig's `codeFor`.
///
/// Only `SlugConflict` leaves the generic bucket (`codeFor` maps
/// `error.SlugConflict, error.AlreadyExists => 6`). Everything else —
/// `SlugNotFound` on an unknown `--scope`, `QueryFailed` on a dangling
/// `--plan` or `--parent` (both are foreign keys) — has no arm there and
/// falls to `else => 1`. All three oracle-confirmed.
/// @param err The engine error.
/// @return The mapped failure.
auto map_task_error(pl::task_error err) -> domain_error {
  auto const kind = err == pl::task_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("task add: {}", zig_error_name(err)));
}

/// @brief Read a boolean flag that carries a declared default of `true`.
///
/// `cliapp::flag_bool` answers "is this key present at all", which is the
/// right question for every flag defaulting to false — but `--editor`
/// defaults to TRUE, so `cliapp::harvest` seeds the key with the literal
/// `"true"` when the flag is absent, and an explicit `--editor=false`
/// lands as the string `"false"` under the SAME present key. `flag_bool`
/// would answer `true` to both. This reads the value.
/// @param args The parsed result.
/// @param name The canonical long name.
/// @param fallback Returned when the flag is absent entirely.
/// @return The flag's value.
auto flag_bool_valued(const cliapp::parsed_args& args, std::string_view name, bool fallback) -> bool {
  auto const raw = cliapp::flag_string(args, name);
  if (!raw.has_value()) {
    return fallback;
  }
  return !(*raw == "false" || *raw == "0");
}

/// @brief Whether this invocation's stdout is the process's real, terminal
/// stdout.
///
/// Both halves matter. `&out == &std::cout` excludes every in-process test,
/// which writes to a `std::ostringstream` while the test binary's own
/// stdout may be a terminal; `isatty` is then the oracle's own condition
/// (`std.Io.File.stdout().isTty(ctx.io) catch false`).
/// @param out The stream the handler will write to.
/// @return `true` only when both hold.
auto stdout_is_tty(std::ostream& out) -> bool {
  return &out == &std::cout && ::isatty(STDOUT_FILENO) == 1;
}

} // namespace

auto task_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before anything else. The oracle opens with `const d = try
  // runtime.ensureDb();`, so even a refused invocation leaves a
  // created-and-migrated database behind. Same ordering, same reason, as
  // `plan create`'s and `assoc create`'s.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};

  auto resolved = resolve_write_scope(ctx, scope_view, "task add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  // NO `project_unassociated` refusal here. The sibling `plan create` has
  // one; this verb deliberately does not — see this module's header for
  // the oracle capture. `resolved->scope` staying unset is the CORRECT
  // outcome inside an unassociated project and lets the engine write
  // `scope_kind='global'`.

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required, so parsing fails first) — but the engine's `title` is a
    // non-optional `std::string`, and defaulting it to "" here would
    // write an untitled row rather than refuse.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "task add: title is required"));
  }

  auto body = cliapp::flag_string(args, "--body");
  if (!body.has_value() && flag_bool_valued(args, "--editor", true) && stdout_is_tty(ctx.out())) {
    // The deferred branch. Refusing rather than proceeding: proceeding
    // would create a real, body-less task and exit 0 on the one path where
    // the operator explicitly asked to type a body. See this module's
    // header.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        "task add: the $EDITOR body flow is not implemented in this build (no process-spawn seam); "
                        "pass `--body <text>` or `--editor=false`"));
  }

  // Every optional argument is threaded EXPLICITLY. `task_create_args`
  // default-constructs all of them, so an omission here is invisible at
  // compile time and shows up only as a NULL column in a row that
  // otherwise looks correct. The `handlers.t.cpp` row assertions exist to
  // catch exactly that.
  auto created = pl::create_task(**conn, pl::task_create_args{
                                             .title = std::move(*title),
                                             .body  = std::move(body),
                                             // No `--status` flag on this verb; the oracle leaves
                                             // `CreateArgs.status` at its `.todo` default.
                                             .status = pl::task_status::todo,
                                             // `--priority` carries a declared default of 100
                                             // (surface.cpp's `k_flags_74`), but read it defensively:
                                             // a caller that harvested an absent flag must still get
                                             // the oracle's default rather than a zero priority.
                                             .priority        = cliapp::flag_int(args, "--priority").value_or(100),
                                             .plan_id         = cliapp::flag_int(args, "--plan"),
                                             .parent_task_id  = cliapp::flag_int(args, "--parent"),
                                             .next_action     = cliapp::flag_string(args, "--next-action"),
                                             .due_at          = cliapp::flag_string(args, "--due"),
                                             .slug            = cliapp::flag_string(args, "--slug"),
                                             .no_auto_promote = cliapp::flag_bool(args, "--no-auto-promote"),
                                             .scope           = resolved->scope,
                                         });
  if (!created) {
    return std::unexpected(map_task_error(created.error()));
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
