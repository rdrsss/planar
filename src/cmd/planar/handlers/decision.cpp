/// @file decision.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.decision`.

module planar.cmd.planar.handlers.decision;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.engine.runtime;
import planar.engine.entitylink;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.scope;

namespace planar::cmd::handlers {

namespace pl       = engine::planning;
namespace session  = engine::runtime::session;
namespace activity = engine::runtime::agentactivity;

namespace {

/// @brief The Zig error name for a `decision_error`.
///
/// zig's handlers die with `exit.die(ctx, e, "decision add: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. `SlugNotFound` was captured from the oracle
/// directly (`decision add --scope nosuchslug`, `decision list --scope
/// nosuchslug`); the rest are transcribed from
/// zig/src/engine/planning/decision.zig's error set, whose members line up
/// one-for-one with this port's `decision_error`.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::decision_error err) -> std::string_view {
  switch (err) {
  case pl::decision_error::not_found:
    return "NotFound";
  case pl::decision_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::decision_error::slug_not_found:
    return "SlugNotFound";
  case pl::decision_error::terminal_status:
    return "TerminalStatus";
  case pl::decision_error::invalid_status:
    return "InvalidStatus";
  case pl::decision_error::query_failed:
    return "QueryFailed";
  case pl::decision_error::link_exists:
    return "LinkExists";
  case pl::decision_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map a `decision_error` onto this binary's exit-code bucket.
///
/// EVERY member lands in the generic bucket (exit 1). None of the Zig tags
/// this family raises has an arm in `zig/src/cmd/planar/exit.zig`'s
/// `codeFor` — not `SlugNotFound`, not `TerminalStatus`, not `LinkExists` —
/// so all of them fall to `else => 1`. Verified by running each against the
/// oracle rather than read off the table. The two refusals that DO exit 2
/// (`--body is required`, `unknown status '<tok>'`) come from the handler's
/// own `invalid_input`, not from here.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"decision add"`.
/// @return The mapped failure.
auto map_decision_error(pl::decision_error err, std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief Map an engine error for a single-id verb, giving `not_found` the
/// oracle's own dedicated message instead of the generic shape.
/// @param err The engine error.
/// @param verb The verb name to lead a generic message with.
/// @param id The decision id, interpolated into the `not_found` message.
/// @return The mapped failure.
auto map_lookup_error(pl::decision_error err, std::string_view verb, std::int64_t id) -> domain_error {
  if (err == pl::decision_error::not_found) {
    // ORACLE: `error: no decision with id 999`, NOT `decision show:
    // NotFound`. Captured from show/accept/withdraw alike.
    return error_from_body(domain_error_kind::not_found, std::format("no decision with id {}", id));
  }
  return map_decision_error(err, verb);
}

/// @brief The vendor identity for the session the create hook attaches to.
///
/// Unset AND empty both yield `cli` — the Zig original's semantics, not
/// "unset -> default". Same discipline as `handlers/question.cpp`'s copy.
/// @param ctx The invocation context.
/// @return The vendor identity.
auto vendor_from(const context& ctx) -> std::string {
  auto const value = ctx.env()("PLANAR_VENDOR");
  if (!value.has_value() || value->empty()) {
    return "cli";
  }
  return *value;
}

/// @brief The vendor's own session id, when it published one.
/// @param ctx The invocation context.
/// @return The vendor session id, or unset.
auto vendor_session_id_from(const context& ctx) -> std::optional<std::string> {
  auto const value = ctx.env()("PLANAR_VENDOR_SESSION_ID");
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

/// @brief Write one decision through the requested renderer.
///
/// Per-renderer terminator contract: `render_text` carries its own trailing
/// newline, `render_json` is a fragment this caller terminates.
/// @param ctx The invocation context.
/// @param args The parsed arguments (read for `--json`).
/// @param d The decision to render.
void emit(context& ctx, const cliapp::parsed_args& args, const pl::decision& d) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(d) << '\n';
  } else {
    ctx.out() << pl::render_text(d);
  }
}

/// @brief Shared body of `decision_accept` / `decision_withdraw`.
///
/// The two differ only in which engine call they make and in the ONE WORD
/// their terminal-status refusal ends with, so the refusal wording is
/// derived from the verb rather than written twice — the alternative is two
/// near-identical string literals that can drift apart silently.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param verb_word The bare verb, `"accept"` or `"withdraw"`.
/// @param apply The engine transition to run.
/// @return Success, or the failure to report.
auto transition_verb(context& ctx, const cliapp::parsed_args& args, std::string_view verb_word,
                     const std::function<std::expected<pl::decision, pl::decision_error>(db::connection&, std::int64_t)>& apply)
    -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "decision-id", "decision");
  if (!id) {
    return std::unexpected(id.error());
  }

  // `--scope` is declared on both leaves and READ BY NEITHER. The oracle
  // discards it (`_ = args.scope;`) and links/transitions anyway, even for
  // a slug that resolves to nothing — captured by running `decision accept
  // 1 --scope nosuchslug`, which succeeds at exit 0.

  auto moved = apply(**conn, *id);
  if (!moved) {
    if (moved.error() == pl::decision_error::terminal_status) {
      // ORACLE: `decision 2 is terminal; cannot accept`. The verb word is
      // the only part that varies.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("decision {} is terminal; cannot {}", *id, verb_word)));
    }
    return std::unexpected(map_lookup_error(moved.error(), std::format("decision {}", verb_word), *id));
  }
  emit(ctx, args, *moved);
  return {};
}

} // namespace

auto decision_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation — zig's handler opens with
  // `try runtime.ensureDb()`, so even a refused invocation leaves a
  // created-and-migrated database behind.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};
  // `map_scope_error` composes `<verb>: resolving scope failed: <Tag>`, so
  // the verb passed here is the bare verb name — zig's add.zig dies with
  // exactly `"decision add: resolving scope failed: {s}"`.
  auto resolved = resolve_write_scope(ctx, scope_view, "decision add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  // NO `project_unassociated` refusal — `resolved->scope` staying unset
  // inside an unassociated project is the CORRECT outcome and lets the
  // engine write `scope_kind='global'`. Confirmed by running `decision add`
  // in a registered-but-unassociated project.

  auto const body = cliapp::flag_string(args, "--body");
  if (!body.has_value()) {
    if (cliapp::flag_bool(args, "--editor")) {
      // The oracle's `--editor` branch opens `$EDITOR` and uses its buffer
      // as the body — but ONLY when stdout is a TTY; on a pipe it falls
      // straight through to this same refusal. This build has no editflow
      // and this `context` has no TTY probe, so the refusal is all it can
      // do. Say so on stderr rather than letting an interactive operator
      // read a flat "--body is required" as the oracle's own answer, which
      // for them it would not be. stdout stays byte-identical either way.
      ctx.err() << "warning: --editor not yet implemented; the interactive editor flow is unported\n";
    }
    // ORACLE, verbatim, exit 2: this fires BEFORE the title is read, so
    // `decision add` with no arguments at all reports the body.
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "--body is required (or run interactively to use the editor flow)"));
  }

  // The session is resolved BEFORE the create and its creation is a
  // COMMITTED SIDE EFFECT even when the create then fails: `decision add
  // --scope nope` exits 1 having written a `sessions` row. Oracle-captured
  // against a scratch root, and identical to `question add`'s ordering.
  auto const session = session::ensure_active(**conn, vendor_from(ctx), vendor_session_id_from(ctx));
  // zig's add.zig writes `ensureActive(...) catch null`, so a failure to
  // start a session is NOT fatal here: the decision is still created, just
  // with a null `session_id` column and no activity hook.
  auto const session_id = session.has_value() ? std::optional<std::int64_t>{*session} : std::optional<std::int64_t>{};

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required), but an absent title must never fall through to "" and
    // write an untitled row.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "decision add: title is required"));
  }
  auto const title_text = *title;

  auto created = pl::create_decision(**conn, pl::decision_create_args{
                                                 .title      = title_text,
                                                 .body       = *body,
                                                 .rationale  = cliapp::flag_string(args, "--rationale"),
                                                 .session_id = session_id,
                                                 .plan_id    = cliapp::flag_int(args, "--plan"),
                                                 .scope      = resolved->scope,
                                             });
  if (!created) {
    return std::unexpected(map_decision_error(created.error(), "decision add"));
  }

  // The entity-create activity hook, composed HERE because the engine
  // cannot reach `engine_runtime` from layer 2 — see decision.cppm's header.
  // Runs after the create has committed and swallows every failure, so it
  // can only add an `agent_actions` row, never change this verb's outcome.
  // A session with no live claim makes it a silent no-op.
  //
  // Note this is a SECOND use of the same session id: the first wrote
  // `decisions.session_id`, which is a column and is not best-effort.
  if (session_id) {
    activity::record_entity_create_action(**conn, *session_id, activity::action_entity_kind::decision, created->id,
                                          std::format("created decision: {}", title_text));
  }

  emit(ctx, args, *created);
  return {};
}

auto decision_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "decision-id", "decision");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto found = pl::show_decision(**conn, *id);
  if (!found) {
    return std::unexpected(map_lookup_error(found.error(), "decision show", *id));
  }
  emit(ctx, args, *found);
  return {};
}

auto decision_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::decision_list_filter filter{};
  filter.plan_id = cliapp::flag_int(args, "--plan");

  // SCOPE IS RESOLVED BEFORE `--status` IS VALIDATED (task 6200). `decision`
  // and `task` are the only two families that order it this way; `plan`,
  // `question` and `scenario` validate the status first. Verified against
  // the oracle per family — see `handlers/task.cpp`'s twin note for the
  // probe matrix and for why the three siblings only APPEAR to agree.
  //
  // Note the targets, which are the MIRROR IMAGE of `question list`'s: an
  // explicit `--scope` fills the SINGULAR field and leaves the vector
  // empty; the cwd-derived read set fills the VECTOR. That is what zig's
  // list.zig does (`.scope = args.scope, .scopes = read_scope_slugs`), and
  // it is also why `--scope` is not split: the singular field is one slug.
  if (auto const raw = cliapp::flag_string(args, "--scope"); raw.has_value()) {
    filter.scope = *raw;
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    filter.scopes = std::move(*slugs);
  }

  // `--status` is SINGLE-VALUED here — NOT comma-split, unlike `plan list`
  // and `question list`. `--status proposed,accepted` is one unknown token.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::decision_status_from_text(*raw);
    if (!st) {
      // Exit 2, not 1: zig's list.zig dies with `error.InvalidInput`, which
      // maps to 2. The same refusal on `question list` exits 1, because
      // there it comes from the engine's `error.InvalidStatus`. Both
      // captured.
      //
      // THIS EXIT CODE IS CORRECT AND MUST NOT BE "FIXED" TO 1. Task 6201
      // was filed against it on the strength of the state-differential's
      // `decision list --status ""` step, which runs from OUTSIDE any
      // registered scope and showed cpp=2 / zig=1. Re-probed in a pinned
      // arena INSIDE a scope, the oracle exits 2 here exactly as this tree
      // does; the differential's zig=1 was the SCOPE error, which is what
      // the reordering above now returns on that step too. Changing this
      // arm to exit 1 would introduce a fresh divergence on every in-scope
      // invocation. See this cycle's report and decision_leaves.t.cpp.
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown status '{}'", *raw)));
    }
    filter.status = *st;
  }

  auto rows = pl::list_decisions(**conn, filter);
  if (!rows) {
    return std::unexpected(map_decision_error(rows.error(), "decision list"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto decision_accept(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return transition_verb(ctx, args, "accept",
                         [](db::connection& conn, std::int64_t id) { return pl::accept_decision(conn, id); });
}

auto decision_withdraw(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return transition_verb(ctx, args, "withdraw",
                         [](db::connection& conn, std::int64_t id) { return pl::withdraw_decision(conn, id); });
}

auto decision_supersede(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const old_id = entity_id_arg(args, "decision-id", "decision");
  if (!old_id) {
    return std::unexpected(old_id.error());
  }

  auto const by = cliapp::flag_int(args, "--by");
  if (!by.has_value()) {
    // `--by` is declared REQUIRED in the surface, so the parser refuses
    // first and this is unreachable — but an absent `--by` must never fall
    // through to 0 and supersede decision 0.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--by is required"));
  }
  auto const new_id = *by;

  // `--scope` declared and ignored, as on accept/withdraw.

  auto superseded = pl::supersede_decision(**conn, *old_id, new_id);
  if (!superseded) {
    switch (superseded.error()) {
    case pl::decision_error::not_found:
      // ORACLE names BOTH ids, because either one could be the missing
      // side and the verb does not say which.
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("no decision with id {} or {}", *old_id, new_id)));
    case pl::decision_error::terminal_status:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("decision {} is terminal; cannot supersede", *old_id)));
    case pl::decision_error::link_exists:
      // Note the ARGUMENT ORDER: the message reads new-to-old, the
      // direction of the `supersedes` edge, which is the reverse of the
      // verb's own `<old> --by <new>`.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure,
                          std::format("supersedes link from decision {} to {} already exists", new_id, *old_id)));
    default:
      return std::unexpected(map_decision_error(superseded.error(), "decision supersede"));
    }
  }
  // The OLD decision is what is rendered — the one whose status changed.
  emit(ctx, args, *superseded);
  return {};
}

auto decision_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow —
  // see handlers/links.cppm's header.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::decision, "decision-id", "decision", "decision_id",
                          "decision link", false);
}

} // namespace planar::cmd::handlers
