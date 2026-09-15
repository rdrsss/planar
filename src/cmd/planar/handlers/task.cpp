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
import planar.engine.ingest.materialize;
import planar.engine.ingest.packet;
import planar.engine.planning;
import planar.engine.runtime;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.engine.entitylink;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;

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

  // TASK 6340: `--plan` and `--parent` are resolved BEFORE the insert. Both
  // are foreign keys, so a nonexistent id used to reach SQLite and surface as
  // two raw lines naming neither the flag nor the id:
  //
  //     error: task.create exec failed: StepFailed
  //     error: task add: QueryFailed
  //
  // An operator who fat-fingered a plan id got a driver error and no way to
  // tell which of the two references was wrong.
  for (auto const& [flag, kind, table] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
           {"--plan", "plan", "plans"}, {"--parent", "task", "tasks"}}) {
    auto const referenced = cliapp::flag_int(args, flag);
    if (!referenced.has_value()) {
      continue;
    }
    // `table` is a literal from the list above, never operator input.
    auto probe = (*conn)->prepare(std::format("select 1 from {} where id = ?", table));
    if (!probe) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task add: reference lookup: QueryFailed"));
    }
    if (auto bound = probe->bind_int64(1, *referenced); !bound) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task add: reference lookup: QueryFailed"));
    }
    auto stepped = probe->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task add: reference lookup: QueryFailed"));
    }
    if (*stepped != db::step_result::row) {
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("no {} with id {} for {}", kind, *referenced, flag)));
    }
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

auto task_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace pk = engine::ingest::packet;

  // The database is opened BEFORE the id is parsed, matching the oracle's
  // handler: `ensureDb()` then `parseInt`. The order is only observable when
  // both would fail, but it is observable.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto packet = pk::assemble_task(**conn, *id);
  if (!packet) {
    if (packet.error() == pk::packet_error::task_not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task packet: QueryFailed"));
  }

  // An unready packet is a SUCCESSFUL answer — see task.cppm. Both renderers
  // return complete stdout payloads including their trailing newline.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? pk::render_json(*packet) : pk::render_text(*packet));
  return {};
}

auto task_facts_stage(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace mz = engine::ingest::materialize;
  namespace pk = engine::ingest::packet;

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "task-id", "task");
  if (!id) {
    return std::unexpected(id.error());
  }

  if (auto staged = mz::stage_one_task(**conn, *id); !staged.has_value()) {
    if (staged.error().kind_ == mz::materialize_error_kind::task_not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
    }
    // An unresolvable citation is an operator-fixable authoring error, and the
    // diagnostic naming the task, artifact, locator and available sections is
    // the whole value — surface it rather than a bare failure name.
    if (staged.error().kind_ == mz::materialize_error_kind::invalid_citation) {
      for (auto const& diagnostic : staged.error().citations_) {
        ctx.err() << "  " << diagnostic.describe() << "\n";
      }
      ctx.err() << "  no facts were staged for this task\n";
      // Exit 1, matching `spec ingest --apply` for the identical condition.
      // Exit 2 is reserved for a malformed INVOCATION; an unresolvable
      // citation is well-formed input naming a section that is not there.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task facts stage: InvalidCitation"));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task facts stage: QueryFailed"));
  }

  // Report the POST-STATE, not the fact that a write returned success: the
  // operator's question is "is this task dispatchable now", and the packet is
  // the only authority on that.
  auto packet = pk::assemble_task(**conn, *id);
  if (!packet) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task facts stage: QueryFailed"));
  }
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pk::render_json(*packet);
    return {};
  }
  ctx.out() << std::format("staged routing facts for task {} ({})\n", *id, mz::operator_materializer_version);
  ctx.out() << std::format("  packet ready: {}\n", packet->ready() ? "yes" : "no");
  if (!packet->ready()) {
    ctx.out() << "  remaining:";
    for (auto const& reason : packet->reasons) {
      ctx.out() << " " << pk::reason_name(reason);
    }
    ctx.out() << "\n";
  }
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

  // SCOPE IS RESOLVED BEFORE `--status` IS VALIDATED, and the order is
  // load-bearing (task 6200). Standing outside any registered scope, the
  // oracle reports the SCOPE error even when `--status` is also bad:
  //
  //   $Z task list --status bogus   (no registered scope)
  //     -> exit 1, "error: cwd is not inside any registered Planar scope; ..."
  //   $Z task list --status bogus   (inside a scope)
  //     -> exit 1, "error: unknown status 'bogus'"
  //
  // `task` and `decision` are the ONLY two families that order it this way.
  // `plan`, `question` and `scenario` validate `--status` FIRST and report
  // the status error from outside a scope — verified against the oracle
  // family by family, not inferred. Do NOT "harmonize" this with them: the
  // three siblings only LOOK like they resolve scope first, because their
  // `--status` is comma-split and an empty string yields zero tokens, so
  // the empty case never reaches their validator at all. `--status bogus`
  // separates the two orderings cleanly and the oracle answers differently.
  //
  // Note the asymmetry this preserves: an EXPLICIT `--scope` skips the
  // cwd-derived read set entirely, so the status error still precedes the
  // engine's `SlugNotFound` (`--scope nosuchslug --status bogus` reports
  // the status). That is the oracle's behaviour too, and it falls out of
  // resolving only the cwd-derived branch here.
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

  // ONE status, not a list — unlike `plan list`. `--status todo,doing`
  // legitimately fails here; oracle-confirmed.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::task_status_from_text(*raw);
    if (!st) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
    }
    filter.status = *st;
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

namespace {

/// @brief The checkout `task touches infer` resolves tokens against.
struct infer_repo {
  std::int64_t id = 0;
  std::string  slug;
  std::string  root;
};

/// @brief Resolve the repo to resolve paths against.
///
/// By slug when `--repo` is given, else the project whose `root_path` is a
/// prefix of the operator's cwd. LONGEST prefix wins, so a submodule
/// checkout beats its superproject — that precedence is the whole reason
/// this is not a plain `resolve_repo_slug` call.
///
/// A project registered with an EMPTY `root_path` is `invalid_input` (exit
/// 2), not "not found": the slug matched, so the operator named a real repo
/// and the fault is in its registration.
/// @param ctx The invocation context, read for the cwd.
/// @param conn The open database connection.
/// @param slug_opt The `--repo` value, if given.
/// @return The repo, or the operator-facing failure.
auto resolve_infer_repo(context& ctx, db::connection& conn, std::optional<std::string> const& slug_opt)
    -> std::expected<infer_repo, domain_error> {
  if (slug_opt.has_value()) {
    auto stmt = conn.prepare("select id, slug, root_path from projects where slug = ?");
    if (!stmt) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
    }
    if (auto b = stmt->bind_text(1, *slug_opt); !b) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug_opt)));
    }
    auto root = stmt->column_text(2);
    if (root.empty()) {
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, "repo has no root_path recorded; cannot resolve paths against it"));
    }
    return infer_repo{.id = stmt->column_int64(0), .slug = stmt->column_text(1), .root = std::move(root)};
  }

  auto stmt = conn.prepare("select id, slug, root_path from projects where root_path is not null");
  if (!stmt) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
  }
  const std::string         cwd = ctx.cwd().string();
  std::optional<infer_repo> best;
  for (;;) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo lookup: QueryFailed"));
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto root = stmt->column_text(2);
    // Longest matching prefix wins, so a submodule checkout beats its
    // superproject.
    //
    // THE MATCH IS ON PATH COMPONENTS, not on the raw string (task 6331).
    // A plain `cwd.starts_with(root)` — what the oracle did and what this
    // reproduced — matches `/a/repo2` against a registration at `/a/repo`,
    // so work done in one repository was attributed to a differently-named
    // sibling. Silently: `touches infer` writes the edges and reports
    // success, naming the wrong repo in output an operator has no reason to
    // re-read.
    //
    // The boundary test is what makes it a path prefix: `cwd` must either BE
    // `root`, or continue with a separator. A trailing separator already
    // stored on `root` is tolerated so a registration of `/a/repo/` behaves
    // the same as `/a/repo`.
    if (root.empty()) {
      continue;
    }
    {
      auto const trimmed =
          (root.size() > 1 && root.back() == '/') ? std::string_view{root}.substr(0, root.size() - 1) : std::string_view{root};
      if (!cwd.starts_with(trimmed)) {
        continue;
      }
      if (cwd.size() > trimmed.size() && cwd[trimmed.size()] != '/') {
        continue;
      }
    }
    if (best.has_value() && root.size() <= best->root.size()) {
      continue;
    }
    best = infer_repo{.id = stmt->column_int64(0), .slug = stmt->column_text(1), .root = std::move(root)};
  }
  if (!best.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "no repo matches the current directory; pass --repo <slug>"));
  }
  return *best;
}

/// @brief Render the `--json` payload for `task touches infer`.
///
/// `applied` mirrors the FLAG, not whether anything was written: the oracle
/// emits `"applied":true,"written":0` when `--apply` runs against a task
/// whose every candidate is held back. Captured directly, and pinned.
/// @param inf The inference.
/// @param repo The resolved repo.
/// @param applied Whether `--apply` was passed.
/// @param written The number of path rows written.
/// @param wide Whether `--wide` was passed.
/// @return The complete payload INCLUDING its trailing newline.
auto infer_json(const engine::planning::touchinfer::inference& inf, const infer_repo& repo, bool applied, std::size_t written,
                bool wide) -> std::string {
  namespace ti    = engine::planning::touchinfer;
  std::string out = std::format(R"({{"task_id":{},"repo_id":{},"repo_slug":)", inf.task_id, repo.id);
  json_text::append_json_string(out, repo.slug);
  out += std::format(R"(,"applied":{},"written":{},"review":{},"candidates":[)", applied ? "true" : "false", written,
                     inf.review_count(wide));
  for (std::size_t i = 0; i < inf.candidates.size(); ++i) {
    const auto& c = inf.candidates[i];
    if (i > 0) {
      out += ',';
    }
    out += R"({"token":)";
    json_text::append_json_string(out, c.token);
    out += R"(,"evidence":)";
    json_text::append_json_string(out, ti::to_text(c.evidence_));
    out += R"(,"classification":)";
    json_text::append_json_string(out, ti::to_text(c.classification_));
    out += R"(,"paths":[)";
    for (std::size_t j = 0; j < c.paths.size(); ++j) {
      if (j > 0) {
        out += ',';
      }
      json_text::append_json_string(out, c.paths[j]);
    }
    out += "]}";
  }
  out += "]}\n";
  return out;
}

} // namespace

auto task_touches_infer(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace ti = engine::planning::touchinfer;

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const task_id = entity_id_arg(args, "task-id", "task");
  if (!task_id) {
    return std::unexpected(task_id.error());
  }

  auto repo = resolve_infer_repo(ctx, **conn, cliapp::flag_string(args, "--repo"));
  if (!repo) {
    return std::unexpected(repo.error());
  }

  auto inferred = ti::infer(**conn, *task_id, repo->id, std::filesystem::path(repo->root));
  if (!inferred) {
    if (inferred.error() == ti::infer_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("task {} not found", *task_id)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches infer: QueryFailed"));
  }
  const auto& inf = *inferred;

  auto const apply   = cliapp::flag_bool(args, "--apply");
  auto const wide    = cliapp::flag_bool(args, "--wide");
  auto const as_json = cliapp::flag_bool(args, "--json");

  // ---- apply ------------------------------------------------------------
  //
  // Both writes per path (the repo-level edge and the path rows) must land
  // together, for the reason `touches add --path` wraps them: a partial
  // commit leaves the coarse repo edge without its path row, and
  // recommend-strategy then falls back to the whole-repo signal and
  // serializes a task that should have been eligible.
  //
  // Note the guard is `writable_count > 0`, so `--apply` against a task
  // whose candidates are ALL held back writes NOTHING — not even the repo
  // edge. Verified against the oracle: `--apply` on a `directory`-only task
  // without `--wide` leaves `task touches list` empty on both levels.
  std::size_t written = 0;
  if (apply && inf.writable_count(wide) > 0) {
    auto opened = (*conn)->begin_transaction();
    if (!opened) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches infer: savepoint: QueryFailed"));
    }
    db::transaction tx{std::move(*opened)};

    // The path-touch implies the repo-touch; keep the coarse signal
    // consistent with the fine one. A pre-existing edge is a no-op.
    auto linked = el::add(**conn, el::entity_link_add_args{.from_kind     = el::entity_kind::task,
                                                           .from_id       = *task_id,
                                                           .to_kind       = el::entity_kind::repo,
                                                           .to_id         = repo->id,
                                                           .relationship_ = el::relationship::touches});
    if (!linked && linked.error() != el::entity_link_error::link_exists) {
      // No endpoint_not_found arm in the oracle either: both endpoints are
      // known to exist by here — `infer` read the task and failed with
      // "task N not found" if it was missing, and the repo was resolved
      // from cwd or --repo.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "task touches infer: repo edge: QueryFailed"));
    }

    for (const auto& c : inf.candidates) {
      if (!ti::is_writable(c.classification_, wide)) {
        continue;
      }
      for (const auto& p : c.paths) {
        // `add_touch_path` is `insert or ignore` against
        // `unique(task_id, repo_id, path)`, so re-running is a no-op and two
        // candidates proposing the same path collapse. `written` counts
        // ATTEMPTS, not inserts — a second `--apply` reports the same count
        // it did the first time. Oracle-captured, and pinned.
        if (auto ok = el::add_touch_path(**conn, *task_id, repo->id, p); !ok) {
          return std::unexpected(
              error_from_body(domain_error_kind::generic_failure, std::format("task touches infer: write {}: QueryFailed", p)));
        }
        ++written;
      }
    }

    if (auto committed = tx.commit(); !committed) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "task touches infer: release savepoint: QueryFailed"));
    }
  }

  // ---- report -----------------------------------------------------------
  if (as_json) {
    ctx.out() << infer_json(inf, *repo, apply, written, wide);
    return {};
  }

  ctx.out() << std::format("task:{}  repo:{}  proposed:{}  review:{}  {}\n", *task_id, repo->slug, inf.writable_count(wide),
                           inf.review_count(wide), apply ? "APPLIED" : "preview (nothing written)");

  for (const auto& c : inf.candidates) {
    if (!ti::is_writable(c.classification_, wide)) {
      continue;
    }
    for (const auto& p : c.paths) {
      ctx.out() << std::format("  + {}\n      [{} via {}: {}]\n", p, ti::to_text(c.classification_), ti::to_text(c.evidence_),
                               c.token);
    }
  }

  if (inf.review_count(wide) > 0) {
    ctx.out() << "\nnot written — review:\n";
    std::size_t wide_available = 0;
    for (const auto& c : inf.candidates) {
      if (ti::is_writable(c.classification_, wide)) {
        continue;
      }
      // Show the expansion size for wide candidates: it is the whole basis
      // for judging one. A token that would declare 35 files is a different
      // proposition from one that would declare 2.
      if (!c.paths.empty()) {
        ++wide_available;
        ctx.out() << std::format("  ? {}  [{} via {} — would declare {} path(s)]\n", c.token, ti::to_text(c.classification_),
                                 ti::to_text(c.evidence_), c.paths.size());
      } else {
        ctx.out() << std::format("  ? {}  [{} via {}]\n", c.token, ti::to_text(c.classification_), ti::to_text(c.evidence_));
      }
    }
    if (!wide && wide_available > 0) {
      ctx.out() << std::format("\n  {} directory/basename candidate(s) withheld — add --wide to include them.\n"
                               "  Wide expansion measured NEGATIVE for eligibility: it intersects peers\n"
                               "  and rule 2 drops both, so it can remove tasks that were otherwise fine.\n",
                               wide_available);
    }
  }

  if (!apply && inf.writable_count(wide) > 0) {
    ctx.out() << std::format("\napply with: planar task touches infer {} --apply\n", *task_id);
  }
  return {};
}

auto task_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow —
  // see handlers/links.cppm's header.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::task, "task-id", "task", "task_id", "task link", false);
}

// ---------------------------------------------------------------------------
// CLI DECLARATION (plan 1051, M11.3a, task 6631)
//
// The `task` command tree, declared HERE rather than as `node_spec` data in
// `surface.cpp`. Decision 1068's target is one declaration site per node;
// task 6401's ask is that the site be next to the handler, which for this
// domain is this file.
//
// Transcribed mechanically from the `node_spec` entries this commit removes,
// so the emitted `schema` catalog is byte-identical -- `scripts/
// surface-snapshot.sh verify` is the acceptance signal and it hashes the
// whole catalog.
//
// THE ONE INVARIANT THIS SHAPE MUST HAND-CARRY, which `apply_surface` used
// to do for free and which every remaining M11.3 wave must keep doing by
// hand:
//
//   SIBLING ORDER. CLI11 renders help and this repo's catalog emitter both
//   walk children in INSERTION order, so the order of the `add_subcommand`
//   calls below IS the catalog's `subcommands` array. `apply_surface`'s
//   `reorder_children` pass used to impose it from the spec list.
//
// NOT an invariant, despite appearances: the `require_subcommand(0)` call
// on every group node below is INERT. CLI11's `require_subcommand_min_`
// already defaults to 0, so deleting it leaves a bare `planar task`
// rendering its help page and exiting 0, and the catalog unchanged
// (break-probed at task 6631 iteration 1). The calls are kept as an
// explicit, defensive statement of intent, matching tree.cpp's root node
// and the planar-agent fold -- not because anything depends on them.
// ---------------------------------------------------------------------------

namespace {

/// @brief Declare the `task touches` subcommands.
///
/// Its own function only because the child names collide with `task`'s
/// own (`add`/`list`); the call site below fixes where the group itself
/// sits among its siblings.
/// @param touches The `task touches` group node.
auto declare_task_touches(CLI::App& touches) -> void {
  CLI::App* add = touches.add_subcommand(
      "add", "Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the repo-level "
             "entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list --touches`.\n\n  With "
             "--path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the repo-level edge \xe2\x80\x94 a "
             "path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> is a repo-relative file "
             "path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  path-level declarations for rules "
             "2/3/4 (disjoint touches, migration\n  touched, singleton file touched). Declare path touches per file (repeat\n  "
             "the verb), not as a list.");
  add_string(*add, "--path");
  add_string(*add, "--scope");
  add_json(*add);
  add_positional(*add, "task-id");
  add_positional(*add, "repo-slug");

  CLI::App* infer = touches.add_subcommand(
      "infer",
      "Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, "
      "proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT \xe2\x80\x94 without --apply nothing is written.\n\n  Each "
      "candidate is classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching "
      "path), 'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n\n  Only 'resolved' is "
      "written by default. The wide classifications \xe2\x80\x94\n  directory and basename \xe2\x80\x94 are shown with their "
      "expansion size and\n  withheld unless --wide is passed. Measured over 46 tasks in six real\n  plans, including them "
      "yielded FEWER parallel-eligible tasks (13) than\n  resolved-only (14): a wide set intersects peers, and rule 2 drops "
      "both\n  sides of an overlap, so one loose directory mention can remove tasks\n  that were otherwise eligible.\n\n  "
      "Proposal still resolves ambiguity wide (decision 906) \xe2\x80\x94 a directory\n  expands, a basename yields every match, "
      "nothing unplaceable is\n  invented. What --wide controls is which proposals are WRITTEN.\n\n  --repo <slug> names the "
      "checkout to resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).");
  add_string(*infer, "--repo");
  add_bool(*infer, "--apply");
  add_bool(*infer, "--wide");
  add_json(*infer);
  add_positional(*infer, "task-id");

  CLI::App* list = touches.add_subcommand("list", "List the repo- and path-level touches declared on a task.");
  add_json(*list);
  add_positional(*list, "task-id");

  CLI::App* remove = touches.add_subcommand(
      "remove", "Withdraw a touch declaration.\n\n  Without --path: removes the repo-level entity_links 'touches' edge.\n\n  "
                "With --path <p>: removes ONE path-level task_touch_paths row and leaves\n  the repo edge in place. Deliberately "
                "not symmetric with `touches add`,\n  where a path-touch implies the repo-touch \xe2\x80\x94 withdrawing one "
                "file should\n  not silently drop a repo claim that may carry other paths.\n\n  Removing the repo edge is not a "
                "substitute for --path: the parallel\n  eligibility rules read task_touch_paths directly, so orphaned path "
                "rows\n  keep driving eligibility after their edge is gone.");
  add_string(*remove, "--path");
  add_string(*remove, "--scope");
  add_json(*remove);
  add_positional(*remove, "task-id");
  add_positional(*remove, "repo-slug");
}

/// @brief Declare every child of the `task` group, in catalog order.
/// @param task The `task` group node.
auto declare_task_children(CLI::App& task) -> void {
  CLI::App* add = task.add_subcommand("add", "Create a new task.");
  add_string(*add, "--body");
  add_string(*add, "--scope");
  add_string(*add, "--next-action");
  add_string(*add, "--due");
  add_int(*add, "--plan");
  add_int(*add, "--parent");
  add_string(*add, "--slug");
  add_int_default(*add, "--priority", "100");
  add_bool_default_true(*add, "--editor");
  add_bool(*add, "--no-auto-promote");
  add_json(*add);
  add_positional(*add, "title");

  CLI::App* show = task.add_subcommand("show", "Show full task details.");
  add_json(*show);
  add_positional(*show, "task-id");

  CLI::App* packet = task.add_subcommand("packet", "Compile the authoritative current routing packet for a task.");
  add_json(*packet);
  add_positional(*packet, "task-id");

  CLI::App* list = task.add_subcommand("list", "List tasks.");
  add_string(*list, "--scope");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_int(*list, "--priority-max");
  add_string(*list, "--touches");
  add_json(*list);

  CLI::App* update = task.add_subcommand("update", "Update mutable fields on a task.");
  add_string(*update, "--title");
  add_string(*update, "--body");
  add_string(*update, "--status");
  add_string(*update, "--next-action");
  add_string(*update, "--due");
  add_int(*update, "--priority");
  add_int(*update, "--plan");
  add_string(*update, "--slug");
  add_string(*update, "--scope");
  add_bool(*update, "--force");
  add_string(*update, "--reason");
  add_bool(*update, "--no-auto-promote");
  add_bool(*update, "--editor");
  add_json(*update);
  add_positional(*update, "task-id");

  CLI::App* edit = task.add_subcommand("edit", "Edit a task in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull");
  add_json(*edit);
  add_positional(*edit, "task-id");

  CLI::App* view = task.add_subcommand("view", "View task's workbench file.");
  add_positional(*view, "task-id");

  CLI::App* diff = task.add_subcommand("diff", "Diff task against its database-stored version.");
  add_positional(*diff, "task-id");

  CLI::App* review = task.add_subcommand("review", "Reviewer entry point for task diff.");
  add_bool(*review, "--approve");
  add_bool(*review, "--request-changes");
  add_json(*review);
  add_positional(*review, "task-id");

  CLI::App* done = task.add_subcommand("done", "Mark a task as done (single-arg form; Go supports variadic).");
  add_string(*done, "--scope");
  add_bool(*done, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*done);
  add_positional(*done, "task-id");

  CLI::App* cancel = task.add_subcommand("cancel", "Cancel a task (single-arg form; Go supports variadic).");
  add_string(*cancel, "--scope");
  add_json(*cancel);
  add_positional(*cancel, "task-id");

  CLI::App* block = task.add_subcommand("block", "Mark a task as blocked and record the blocking relationship.");
  add_int_required(*block, "--on", "Blocking task id");
  add_string(*block, "--reason");
  add_string(*block, "--scope");
  add_bool(*block, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*block);
  add_positional(*block, "task-id");

  CLI::App* link = task.add_subcommand("link", "Create an entity link from a task to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "task-id");
  add_positional(*link, "ref");

  CLI::App* reopen = task.add_subcommand("reopen", "Reopen a done or cancelled task with an audit-trail entry.");
  add_string(*reopen, "--status");
  add_string(*reopen, "--reason");
  add_string(*reopen, "--scope");
  add_bool(*reopen, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*reopen);
  add_positional(*reopen, "task-id");

  CLI::App* touches = task.add_subcommand("touches", "Manage repo-touches links on a task.");
  touches->require_subcommand(0);
  declare_task_touches(*touches);

  CLI::App* facts = task.add_subcommand("facts", "Manage routing facts on a task.");
  facts->require_subcommand(0);
  CLI::App* facts_stage = facts->add_subcommand(
      "stage", "Stage this task's routing facts under operator provenance.\n\n  `spec ingest --apply` is the only other "
               "writer of routing facts, and it\n  rebuilds an entire anchor plan, so a hand-filed task could never obtain\n"
               "  them and an edited task could never restage them. This stages exactly\n  one task, stamped `operator-v1`, "
               "and never touches a sibling's facts.\n\n  Citation facts are staged only for artifacts this task already "
               "cites\n  AND references explicitly in its body; it never invents a citation.");
  add_json(*facts_stage);
  add_positional(*facts_stage, "task-id");
}

} // namespace

auto declare_task(CLI::App& root) -> void {
  CLI::App* task = root.add_subcommand(
      "task", "Manage tasks \xe2\x80\x94 the discrete units of work.\n\n  Tasks may belong to a plan (--plan) or another task "
              "(--parent), and\n  carry the next_action field required by resume validate.\n  Status lifecycle: todo "
              "\xe2\x86\x92 doing \xe2\x86\x92 done / cancelled; blocked is set\n  via task block.");
  task->require_subcommand(0);
  declare_task_children(*task);
}

} // namespace planar::cmd::handlers
