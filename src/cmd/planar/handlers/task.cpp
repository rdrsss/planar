/// @file task.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.task`.

module;

#include <unistd.h>

module planar.cmd.planar.handlers.task;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.entitylink;
import planar.engine.identity;
import planar.engine.planning;
import planar.engine.runtime;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.engine.entitylink;
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
  case pl::task_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
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

/// @brief `map_task_error` for a verb other than `task add`, which leads its
/// message with its own verb name.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"task done"`.
/// @return The mapped failure.
auto map_task_error_for(pl::task_error err, std::string_view verb) -> domain_error {
  auto const kind = err == pl::task_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief The oracle's TWO-LINE active-claim refusal, verbatim.
///
/// Four of the five status-flip verbs print this; `task cancel` does NOT —
/// its handler has no `error.TaskClaimed` arm, so its refusal falls through
/// to the generic `task cancel: TaskClaimed`. Both captured against a live
/// claim rather than reasoned about.
///
/// `rendered` stays false: this is a message BODY, so the reporting site
/// composes the `error: ` prefix and the terminator around it. The embedded
/// newline is inside the body, which is why the second line has no prefix.
/// @param id The claimed task's row id.
/// @return The refusal (exit 1 — `error.TaskClaimed` has no arm in `codeFor`).
auto task_claimed_error(std::int64_t id) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure,
                         std::format("task {} has an active work claim — operator status flip refused.\n"
                                     "Release or complete the claim via the agent path, or re-run with --force to override.",
                                     id));
}

/// @brief Run the active-work-claim guard, composed at layer 3.
///
/// See `agentactivity::has_active_claim_on_task` for why the check lives
/// outside `engine_planning` and what that costs. A query FAILURE is
/// reported as a refusal rather than swallowed: silently proceeding when
/// the guard cannot answer is how a guard becomes decoration.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @param force When true, the guard is skipped entirely (the `--force`
/// override the refusal text advertises).
/// @param generic_verb When set, render the refusal as
/// `"<verb>: TaskClaimed"` instead of the two-line advisory — `task
/// cancel`'s shape.
/// @return Success when the flip may proceed, else the refusal.
auto check_task_claim(db::connection& conn, std::int64_t id, bool force, std::optional<std::string_view> generic_verb)
    -> std::expected<void, domain_error> {
  if (force) {
    return {};
  }
  auto held = engine::runtime::agentactivity::has_active_claim_on_task(conn, id);
  if (!held) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "checking active work claims: QueryFailed"));
  }
  if (!*held) {
    return {};
  }
  if (generic_verb.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("{}: TaskClaimed", *generic_verb)));
  }
  return std::unexpected(task_claimed_error(id));
}

/// @brief Run the cross-scope write guard for `task update`.
///
/// The only task verb that carries it — `done`, `cancel`, `block` and
/// `reopen` all declare `--scope` and then never read it. That is NOT an
/// oversight to be tidied up: those four handlers genuinely ignore the flag
/// in the oracle, so a `--scope` passed to them is inert, and reproducing
/// the inertness is what keeps this binary's behaviour identical. It is the
/// same shape as `planar-watch ps --vendor`, and it is pinned as a
/// reproduced quirk rather than fixed (D2).
/// @param conn An open, migrated database connection.
/// @param ctx The invocation context (for cwd derivation).
/// @param id The task's row id.
/// @param scope_flag The raw `--scope` value, when passed.
/// @return Success when the write is allowed, else the exit-5 refusal.
auto check_task_scope(context& ctx, db::connection& conn, std::int64_t id, std::optional<std::string_view> scope_flag)
    -> std::expected<void, domain_error> {
  auto current = pl::show_task(conn, id);
  if (!current) {
    if (current.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", id)));
    }
    return std::unexpected(map_task_error_for(current.error(), "task lookup"));
  }

  auto const kind = [&] {
    switch (current->scope_kind) {
    case pl::task_scope_kind::global:
      return engine::identity::scope_kind::global;
    case pl::task_scope_kind::association:
      return engine::identity::scope_kind::association;
    case pl::task_scope_kind::repo:
      break;
    }
    return engine::identity::scope_kind::repo;
  }();

  auto entity_scope = engine::identity::slug_from_ref(conn, kind, current->scope_id);
  if (!entity_scope) {
    return std::unexpected(map_scope_error(entity_scope.error(), "task update: looking up entity scope"));
  }

  auto resolved = resolve_write_scope(ctx, scope_flag, "task update: resolving write scope");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  auto const entity_view = entity_scope->has_value() ? std::optional<std::string_view>{**entity_scope} : std::nullopt;
  auto const write_view  = resolved->scope.has_value() ? std::optional<std::string_view>{*resolved->scope} : std::nullopt;
  if (engine::identity::check_scope_guard(entity_view, write_view)) {
    return {};
  }
  // The message names the entity's scope THREE times — twice as the scope
  // it is in and once as the `--scope` that would allow the write — and
  // renders an unset scope as the literal "global" in every position.
  // Oracle-captured.
  auto const entity_label = entity_scope->has_value() ? **entity_scope : std::string{"global"};
  auto const write_label  = resolved->scope.value_or("global");
  return std::unexpected(error_from_body(
      domain_error_kind::scope_mismatch,
      std::format("scope mismatch: task {} is in scope '{}' but operator write scope is '{}'; pass --scope {} to write to "
                  "that scope from here",
                  id, entity_label, write_label, entity_label)));
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

namespace {

/// @brief Write one task row in whichever form `--json` selects.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param t The row to render.
void emit_task(context& ctx, const cliapp::parsed_args& args, const pl::task& t) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(t) << '\n';
  } else {
    ctx.out() << pl::render_text(t);
  }
}

} // namespace

auto task_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto found = pl::show_task(**conn, *id);
  if (!found) {
    if (found.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(found.error(), "task show"));
  }
  emit_task(ctx, args, *found);
  return {};
}

namespace {

/// @brief Forward declaration of the repo-slug resolver defined with the
/// `task touches` block below.
///
/// `task list --touches` needs it and is defined ABOVE that block; an
/// anonymous namespace re-opened later in the same translation unit is the
/// SAME namespace, so this declares the one entity rather than a second.
/// @param conn An open, migrated database connection.
/// @param slug The repo slug.
/// @return The row id, `std::nullopt` when absent, or the refusal.
auto resolve_repo_slug(db::connection& conn, std::string_view slug) -> std::expected<std::optional<std::int64_t>, domain_error>;

} // namespace

auto task_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::task_list_filter filter{};
  filter.plan_id      = cliapp::flag_int(args, "--plan");
  filter.priority_max = cliapp::flag_int(args, "--priority-max");

  // ONE status, not a list — unlike `plan list`. `--status todo,doing`
  // legitimately fails here; oracle-confirmed.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::task_status_from_text(*raw);
    if (!st) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
    }
    filter.status = *st;
  }

  if (auto raw = cliapp::flag_string(args, "--scope"); raw.has_value()) {
    // NOT comma-split, unlike `plan list --scope`. The oracle hands the raw
    // string to the engine as ONE slug, so `--scope a,b` reports
    // `SlugNotFound`. Confirmed by running both verbs side by side; the
    // asymmetry is real and reproducing it is the point.
    filter.scope = std::move(*raw);
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    filter.scopes = std::move(*slugs);
  }

  // `--touches <repo-slug>` swaps the engine call for the UNION query
  // rather than post-filtering the unfiltered list: the two branches
  // (direct repo scope, touches edge) apply the scope predicate
  // DIFFERENTLY, and a post-filter cannot express that. Refused at exit 64
  // until task 6187 landed `list_tasks_touching`.
  std::expected<std::vector<pl::task>, pl::task_error> rows;
  if (auto const slug = cliapp::flag_string(args, "--touches"); slug.has_value()) {
    auto repo_id = resolve_repo_slug(**conn, *slug);
    if (!repo_id) {
      return std::unexpected(repo_id.error());
    }
    if (!repo_id->has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug)));
    }
    rows = pl::list_tasks_touching(**conn, **repo_id, filter);
    if (!rows) {
      return std::unexpected(map_task_error_for(rows.error(), "task list --touches"));
    }
  } else {
    rows = pl::list_tasks(**conn, filter);
    if (!rows) {
      return std::unexpected(map_task_error_for(rows.error(), "task list"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto task_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};

  // FIRST guard: cross-scope. Runs before the patch is even built, and
  // before the claim guard, because that is the oracle's order — a
  // cross-scope update of a CLAIMED task reports the scope mismatch, not
  // the claim.
  if (auto guarded = check_task_scope(ctx, **conn, *id, scope_view); !guarded) {
    return std::unexpected(guarded.error());
  }

  pl::task_update_args patch{};
  patch.title           = cliapp::flag_string(args, "--title");
  patch.body            = cliapp::flag_string(args, "--body");
  patch.priority        = cliapp::flag_int(args, "--priority");
  patch.next_action     = cliapp::flag_string(args, "--next-action");
  patch.due_at          = cliapp::flag_string(args, "--due");
  patch.slug            = cliapp::flag_string(args, "--slug");
  patch.no_auto_promote = cliapp::flag_bool(args, "--no-auto-promote");
  patch.scope           = scope_flag;
  patch.force           = cliapp::flag_bool(args, "--force");
  patch.reason          = cliapp::flag_string(args, "--reason");

  // `--plan 0` is the CLEAR sentinel, same shape as `plan update --parent 0`.
  if (auto const plan = cliapp::flag_int(args, "--plan"); plan.has_value()) {
    if (*plan == 0) {
      patch.clear_plan = true;
    } else {
      patch.plan_id = *plan;
    }
  }

  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::task_status_from_text(*raw);
    if (!st) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
    }
    patch.status = *st;
  }

  // SECOND guard: the active claim, and ONLY on a status change. A
  // title/body/priority patch on a claimed task is allowed — oracle-
  // confirmed by running `task update <claimed> --title X`, which exits 0.
  // Gating unconditionally here would refuse a write the oracle permits.
  if (patch.status.has_value()) {
    if (auto guarded = check_task_claim(**conn, *id, patch.force, std::nullopt); !guarded) {
      return std::unexpected(guarded.error());
    }
  }

  // `--editor` is DECLARED on this verb (default false) and never read by
  // the oracle's handler — unlike `task add`, where it defaults true and
  // drives the $EDITOR body flow. Left inert deliberately; see
  // `check_task_scope`'s note on reproduced declared-but-ignored flags.

  auto updated = pl::update_task(**conn, *id, patch);
  if (!updated) {
    if (updated.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(updated.error(), "task update"));
  }
  emit_task(ctx, args, *updated);
  return {};
}

auto task_done(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto const force = cliapp::flag_bool(args, "--force");

  if (auto guarded = check_task_claim(**conn, *id, force, std::nullopt); !guarded) {
    return std::unexpected(guarded.error());
  }

  auto updated = pl::mark_done(**conn, *id, force);
  if (!updated) {
    if (updated.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(updated.error(), "task done"));
  }
  emit_task(ctx, args, *updated);
  return {};
}

auto task_cancel(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  // No `--force` on this verb at all, and the refusal renders in the
  // GENERIC shape. Both are the oracle's; see this verb's declaration.
  if (auto guarded = check_task_claim(**conn, *id, false, std::string_view{"task cancel"}); !guarded) {
    return std::unexpected(guarded.error());
  }

  auto updated = pl::mark_cancelled(**conn, *id);
  if (!updated) {
    if (updated.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(updated.error(), "task cancel"));
  }
  emit_task(ctx, args, *updated);
  return {};
}

auto task_block(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto const on = cliapp::flag_int(args, "--on");
  if (!on.has_value()) {
    // Declared required, so unreachable through the tree — but defaulting
    // to 0 would insert a `depends-on` edge to a task that does not exist.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--on is required"));
  }
  auto const force  = cliapp::flag_bool(args, "--force");
  auto const reason = cliapp::flag_string(args, "--reason");

  if (auto guarded = check_task_claim(**conn, *id, force, std::nullopt); !guarded) {
    return std::unexpected(guarded.error());
  }

  auto const reason_view = reason.has_value() ? std::optional<std::string_view>{*reason} : std::optional<std::string_view>{};
  auto       updated     = pl::mark_blocked(**conn, *id, *on, reason_view, force);
  if (!updated) {
    if (updated.error() == pl::task_error::not_found) {
      // NOTE: `not_found` here can name EITHER task — the one being blocked
      // or the blocker. The oracle interpolates the subject id in both
      // cases, which is misleading for a bad `--on`, and is reproduced.
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(updated.error(), "task block"));
  }
  emit_task(ctx, args, *updated);
  return {};
}

auto task_reopen(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  // `--reason` is declared OPTIONAL by the tree and required by the
  // handler. Defaulting it to "" would write an empty `task_reopens.reason`
  // — an audit row that records nothing, which is worse than a refusal.
  auto const reason = cliapp::flag_string(args, "--reason");
  if (!reason.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--reason is required for reopen"));
  }

  auto status = pl::task_status::todo;
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const parsed = pl::task_status_from_text(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
    }
    status = *parsed;
  }

  auto const force = cliapp::flag_bool(args, "--force");
  if (auto guarded = check_task_claim(**conn, *id, force, std::nullopt); !guarded) {
    return std::unexpected(guarded.error());
  }

  // The engine's `reopen` takes no `force`: in the oracle that parameter
  // gates ONLY the claim check, which is now composed above, so there is
  // nothing left for it to do inside the engine. It is not a dropped
  // argument.
  auto updated = pl::reopen(**conn, *id, status, *reason);
  if (!updated) {
    if (updated.error() == pl::task_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(map_task_error_for(updated.error(), "task reopen"));
  }
  emit_task(ctx, args, *updated);
  return {};
}

// =========================================================================
// `task touches` — the repo- and path-level declaration surface (task 6187).
//
// The engine halves were ALREADY ported and are NOT in `engine_planning`:
// `add_touch_path` / `remove_touch_path` / `touched_paths` /
// `touched_repo_ids`, plus generic `entity_links` add/list/remove, all live
// in the layer-2 `planar.engine.entitylink` bucket. This cycle's brief
// asserted the whole `task touches` surface had "no engine"; that was
// checked against `entitylink.cppm` before any code was written and found
// to be true only of `listTouching` (now in `engine_planning`) and of the
// `infer` leaf. The rest is composition at layer 3, which is where it has
// to happen anyway: `engine_entitylink` and `engine_planning` are both
// layer 2 and `cmake/architecture.cmake` FATALs on an edge between them.
// =========================================================================

namespace {

namespace el = engine::entitylink;

/// @brief Resolve a repo slug to a `projects.id`.
///
/// A direct query rather than `entitylink::parse_ref`, which does not do
/// slug lookup for the `repo` kind — `projects` carries the slug column but
/// is not in the ref resolver's table. Same reason the oracle's handlers
/// each carry their own `resolveRepoSlug`.
/// @param conn An open, migrated database connection.
/// @param slug The repo slug.
/// @return The row id, `std::nullopt` when no such project exists, or the
/// error when the query itself failed.
auto resolve_repo_slug(db::connection& conn, std::string_view slug) -> std::expected<std::optional<std::int64_t>, domain_error> {
  auto stmt = conn.prepare("select id from projects where slug = ?");
  if (!stmt) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
  }
  if (auto b = stmt->bind_text(1, slug); !b) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
  }
  if (*stepped == db::step_result::done) {
    return std::optional<std::int64_t>{};
  }
  return std::optional<std::int64_t>{stmt->column_int64(0)};
}

/// @brief Warn — never refuse — when the named repo belongs to no association.
///
/// Link verbs are unguarded on purpose: a touches edge into a DIFFERENT
/// association is the legitimate polyrepo workflow, so scope disagreement
/// is not an error. A repo in NO association is a different thing —
/// nothing can reach it, so it is nearly always a stale duplicate slug
/// picked over the live one, and declarations pile up against a root_path
/// that is not the operator's checkout.
///
/// Every failure here is swallowed: a diagnostic must not turn a valid
/// write into an error. Suppressed under `--json`, matching the oracle,
/// so machine consumers get a clean stream.
/// @param ctx The invocation context (the advisory goes to `ctx.err()`).
/// @param conn An open, migrated database connection.
/// @param repo_id The repo being declared against.
/// @param slug The slug as the operator spelled it.
void warn_if_orphan_repo(context& ctx, db::connection& conn, std::int64_t repo_id, std::string_view slug) {
  auto stmt = conn.prepare("select count(*) from project_associations where project_id = ?");
  if (!stmt) {
    return;
  }
  if (auto b = stmt->bind_int64(1, repo_id); !b) {
    return;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row || stmt->column_int64(0) != 0) {
    return;
  }
  ctx.err() << std::format("warning: repo '{}' belongs to no association — declarations on it are unreachable from any "
                           "scope, and closure extraction will read its root_path rather than your checkout's\n",
                           slug);

  // Offer the likely intended repo: another project whose root_path has the
  // same basename and which IS associated (the `planar` / `planar-2` shape).
  auto alt = conn.prepare("select b.slug from projects a "
                          "join projects b on b.id <> a.id "
                          "where a.id = ? "
                          "  and b.root_path is not null and a.root_path is not null "
                          "  and replace(b.root_path, rtrim(b.root_path, replace(b.root_path, '/', '')), '') "
                          "      = replace(a.root_path, rtrim(a.root_path, replace(a.root_path, '/', '')), '') "
                          "  and exists (select 1 from project_associations pa where pa.project_id = b.id) "
                          "limit 1");
  if (!alt) {
    return;
  }
  if (auto b = alt->bind_int64(1, repo_id); !b) {
    return;
  }
  auto alt_stepped = alt->step();
  if (!alt_stepped || *alt_stepped != db::step_result::row) {
    return;
  }
  ctx.err() << std::format("         did you mean '{}'?\n", alt->column_text(0));
}

/// @brief Render the shared `add` / `remove` JSON result.
///
/// `path` is ALWAYS emitted, as `null` in repo-level mode rather than
/// omitted — the oracle's struct declares it optional-with-default and its
/// serializer writes the key regardless. A consumer keying on the field's
/// PRESENCE to tell the two modes apart would break if it were dropped.
/// @param task_id The task.
/// @param repo_id The resolved repo id.
/// @param repo_slug The slug as spelled by the operator.
/// @param path The declared path, or unset in repo-level mode.
/// @return The JSON object, with no trailing newline.
auto touches_result_json(std::int64_t task_id, std::int64_t repo_id, std::string_view repo_slug,
                         const std::optional<std::string>& path) -> std::string {
  return std::format(R"({{"ok":true,"task_id":{},"repo_id":{},"repo_slug":{},"path":{}}})", task_id, repo_id,
                     json_text::json_string(repo_slug), path.has_value() ? json_text::json_string(*path) : std::string{"null"});
}

/// @brief Resolve the `<task-id> <repo-slug>` pair `add` and `remove` share.
/// @param ctx The invocation context.
/// @param conn An open, migrated database connection.
/// @param args The parsed arguments.
/// @return The task id, repo id and slug, or the refusal.
auto touches_endpoints(context& ctx, db::connection& conn, const cliapp::parsed_args& args)
    -> std::expected<std::tuple<std::int64_t, std::int64_t, std::string>, domain_error> {
  auto const task_id = entity_id_arg(args, "task-id", "task");
  if (!task_id) {
    return std::unexpected(task_id.error());
  }
  auto const slug = cliapp::positional_string(args, "repo-slug");
  if (!slug.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "repo-slug is required"));
  }
  auto resolved = resolve_repo_slug(conn, *slug);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  if (!resolved->has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug)));
  }
  (void)ctx;
  return std::make_tuple(*task_id, **resolved, *slug);
}

} // namespace

auto task_touches_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto endpoints = touches_endpoints(ctx, **conn, args);
  if (!endpoints) {
    return std::unexpected(endpoints.error());
  }
  auto const [task_id, repo_id, slug] = *endpoints;
  auto const path                     = cliapp::flag_string(args, "--path");
  auto const as_json                  = cliapp::flag_bool(args, "--json");

  if (!as_json) {
    warn_if_orphan_repo(ctx, **conn, repo_id, slug);
  }

  // Path mode performs TWO writes and they must land together. A partial
  // pair (edge present, path row missing) degrades the parallelizability
  // rules to the coarse whole-repo signal without any error surfacing.
  std::optional<db::transaction> tx;
  if (path.has_value()) {
    auto opened = (*conn)->begin_transaction();
    if (!opened) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches add: savepoint: QueryFailed"));
    }
    tx.emplace(std::move(*opened));
  }

  auto linked = el::add(**conn, el::entity_link_add_args{.from_kind     = el::entity_kind::task,
                                                         .from_id       = task_id,
                                                         .to_kind       = el::entity_kind::repo,
                                                         .to_id         = repo_id,
                                                         .relationship_ = el::relationship::touches});
  if (!linked) {
    switch (linked.error()) {
    case el::entity_link_error::link_exists:
      // In path mode a pre-existing edge is EXPECTED — the path implies it.
      // In repo-only mode it is the refusal.
      if (!path.has_value()) {
        return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                               std::format("touches link task:{} -> repo:{} already exists", task_id, slug)));
      }
      break;
    case el::entity_link_error::endpoint_not_found:
      // Only the TASK side can be missing: the repo id came from a
      // successful slug lookup moments ago.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("task:{} not found", task_id)));
    case el::entity_link_error::unsupported_scope:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "scoped entity links not yet supported (M3)"));
    default:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches add: QueryFailed"));
    }
  }

  if (path.has_value()) {
    if (auto written = el::add_touch_path(**conn, task_id, repo_id, *path); !written) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches add --path: QueryFailed"));
    }
    if (auto committed = tx->commit(); !committed) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "task touches add: release savepoint: QueryFailed"));
    }
  }

  if (as_json) {
    ctx.out() << touches_result_json(task_id, repo_id, slug, path) << '\n';
  } else if (path.has_value()) {
    ctx.out() << std::format("path-touch added: task:{} -> repo:{} path:{}\n", task_id, slug, *path);
  } else {
    ctx.out() << std::format("touches link added: task:{} -> repo:{}\n", task_id, slug);
  }
  return {};
}

auto task_touches_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const task_id = entity_id_arg(args, "task-id", "task");
  if (!task_id) {
    return std::unexpected(task_id.error());
  }

  // Ordered by SLUG, not by link id — so this listing and
  // `entitylink::touched_repo_ids` (ordered by link id) can disagree on
  // order for the same task. The oracle's `task touches list` does its own
  // slug-joined query rather than calling the engine helper, and that is
  // reproduced here for the same reason: the operator-facing listing is
  // alphabetical.
  std::vector<std::string> repos;
  {
    auto stmt = (*conn)->prepare("select p.slug from entity_links el join projects p on p.id = el.to_id "
                                 "where el.from_kind = 'task' and el.from_id = ? "
                                 "  and el.to_kind = 'repo' and el.relationship = 'touches' order by p.slug");
    if (!stmt) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list (repos): QueryFailed"));
    }
    if (auto b = stmt->bind_int64(1, *task_id); !b) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list bind: QueryFailed"));
    }
    for (;;) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list step: QueryFailed"));
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      repos.push_back(stmt->column_text(0));
    }
  }

  std::vector<std::pair<std::string, std::string>> paths;
  {
    auto stmt = (*conn)->prepare("select p.slug, ttp.path from task_touch_paths ttp "
                                 "join projects p on p.id = ttp.repo_id "
                                 "where ttp.task_id = ? order by p.slug, ttp.path");
    if (!stmt) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list (paths): QueryFailed"));
    }
    if (auto b = stmt->bind_int64(1, *task_id); !b) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list bind: QueryFailed"));
    }
    for (;;) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches list step: QueryFailed"));
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      paths.emplace_back(stmt->column_text(0), stmt->column_text(1));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    std::string out = std::format(R"({{"task_id":{},"repos":[)", *task_id);
    for (std::size_t i = 0; i < repos.size(); ++i) {
      if (i > 0) {
        out += ",";
      }
      out += json_text::json_string(repos[i]);
    }
    out += R"(],"paths":[)";
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (i > 0) {
        out += ",";
      }
      out += std::format(R"({{"repo":{},"path":{}}})", json_text::json_string(paths[i].first),
                         json_text::json_string(paths[i].second));
    }
    out += "]}";
    ctx.out() << out << '\n';
    return {};
  }

  ctx.out() << std::format("task:{} touches\n", *task_id);
  if (repos.empty() && paths.empty()) {
    ctx.out() << "  (none declared)\n";
    return {};
  }
  for (const auto& slug : repos) {
    ctx.out() << std::format("  repo: {}\n", slug);
  }
  for (const auto& row : paths) {
    ctx.out() << std::format("  path: {}:{}\n", row.first, row.second);
  }
  return {};
}

auto task_touches_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto endpoints = touches_endpoints(ctx, **conn, args);
  if (!endpoints) {
    return std::unexpected(endpoints.error());
  }
  auto const [task_id, repo_id, slug] = *endpoints;
  auto const path                     = cliapp::flag_string(args, "--path");
  auto const as_json                  = cliapp::flag_bool(args, "--json");

  if (path.has_value()) {
    // Withdraws the PATH row only; the repo edge is deliberately left in
    // place. See the declaration's documentation.
    auto removed = el::remove_touch_path(**conn, task_id, repo_id, *path);
    if (!removed) {
      if (removed.error() == el::entity_link_error::not_found) {
        return std::unexpected(
            error_from_body(domain_error_kind::generic_failure,
                            std::format("task:{} has no declared path touch '{}' on repo:{}", task_id, *path, slug)));
      }
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches remove --path: QueryFailed"));
    }
    if (as_json) {
      ctx.out() << touches_result_json(task_id, repo_id, slug, path) << '\n';
    } else {
      ctx.out() << std::format("path-touch removed: task:{} -> repo:{} path:{}\n", task_id, slug, *path);
    }
    return {};
  }

  auto links = el::list(**conn, el::entity_link_list_filter{.from_kind     = el::entity_kind::task,
                                                            .from_id       = task_id,
                                                            .to_kind       = el::entity_kind::repo,
                                                            .to_id         = repo_id,
                                                            .relationship_ = el::relationship::touches});
  if (!links) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches remove lookup: QueryFailed"));
  }
  if (links->empty()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("no touches link between task:{} and repo:{}", task_id, slug)));
  }
  if (auto dropped = el::remove(**conn, links->front().id); !dropped) {
    if (dropped.error() == el::entity_link_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "link not found (already removed?)"));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches remove: QueryFailed"));
  }

  if (as_json) {
    ctx.out() << touches_result_json(task_id, repo_id, slug, std::nullopt) << '\n';
  } else {
    // U+2192 here, but ASCII `->` in `add`'s line. Both oracle-captured
    // against the same database; the asymmetry is real and is pinned.
    ctx.out() << std::format("touches link removed: task:{} → repo:{}\n", task_id, slug);
  }
  return {};
}

auto task_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow —
  // see handlers/links.cppm's header.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::task, "task-id", "task", "task_id", "task link", false);
}

} // namespace planar::cmd::handlers
