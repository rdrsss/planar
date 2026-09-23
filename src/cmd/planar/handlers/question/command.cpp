/// @file question.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.question`.

module planar.cmd.planar.handlers.question;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.engine.runtime;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.engine.entitylink;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.question.add;
import planar.cmd.planar.handlers.question.edit;
import planar.cmd.planar.handlers.question.view;
import planar.cmd.planar.handlers.question.diff;
import planar.cmd.planar.handlers.question.review;
import planar.cmd.planar.handlers.question.answer;
import planar.cmd.planar.handlers.question.wontfix;
import planar.cmd.planar.handlers.question.list;
import planar.cmd.planar.handlers.question.show;
import planar.cmd.planar.handlers.question.link;

namespace planar::cmd::handlers {

namespace pl       = engine::planning;
namespace session  = engine::runtime::session;
namespace activity = engine::runtime::agentactivity;

namespace {

/// @brief The Zig error name for a `question_error`.
///
/// zig's handlers die with `exit.die(ctx, e, "question add: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. `SlugNotFound` and `IllegalTransition` were
/// captured from the oracle directly; the rest are transcribed from
/// zig/src/engine/planning/question.zig's error set, whose members line up
/// one-for-one with this port's `question_error`.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::question_error err) -> std::string_view {
  switch (err) {
  case pl::question_error::not_found:
    return "NotFound";
  case pl::question_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::question_error::slug_not_found:
    return "SlugNotFound";
  case pl::question_error::query_failed:
    return "QueryFailed";
  case pl::question_error::answer_required:
    return "AnswerRequired";
  case pl::question_error::illegal_transition:
    return "IllegalTransition";
  case pl::question_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map a `question_error` onto this binary's exit-code bucket.
///
/// EVERY member lands in the generic bucket (exit 1). None of the Zig tags
/// this family raises has an arm in `zig/src/cmd/planar/exit.zig`'s
/// `codeFor` — not `SlugNotFound`, not `AnswerRequired`, not
/// `IllegalTransition` — so all of them fall to `else => 1`. Verified by
/// running each against the oracle rather than read off the table; the
/// `--answer is required` refusal, which DOES exit 2, comes from the
/// handler's own `invalid_input`, not from here.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"question add"`.
/// @return The mapped failure.
auto map_question_error(pl::question_error err, std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief Map an engine error for a verb whose `not_found` arm carries the
/// oracle's own dedicated message instead of the generic shape.
/// @param err The engine error.
/// @param verb The verb name to lead a generic message with.
/// @param id The question id, interpolated into the `not_found` message.
/// @return The mapped failure.
auto map_lookup_error(pl::question_error err, std::string_view verb, std::int64_t id) -> domain_error {
  if (err == pl::question_error::not_found) {
    // ORACLE: `error: no question with id 999`, NOT `question show:
    // NotFound`. Captured from all three lookup verbs.
    return error_from_body(domain_error_kind::not_found, std::format("no question with id {}", id));
  }
  return map_question_error(err, verb);
}

/// @brief Split a comma-separated flag value the way the oracle's
/// `question list --status` / `--scope` do: split on `,`, trim ASCII spaces
/// from each token, DROP empty tokens.
///
/// A verbatim copy of `handlers/plan.cpp`'s helper. It is duplicated rather
/// than extracted because the two live in different translation units and
/// the rule is four lines of vocabulary, not a policy — the same call the
/// tree already made for `resolve_repo_slug_for_touches`.
/// @param raw The flag's raw value.
/// @return The non-empty, trimmed tokens, in order.
auto split_csv(std::string_view raw) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const part : std::views::split(raw, ',')) {
    std::string_view tok{part.begin(), part.end()};
    while (!tok.empty() && tok.front() == ' ') {
      tok.remove_prefix(1);
    }
    while (!tok.empty() && tok.back() == ' ') {
      tok.remove_suffix(1);
    }
    if (!tok.empty()) {
      out.emplace_back(tok);
    }
  }
  return out;
}

/// @brief Resolve a repo slug to a `projects.id` for `question list
/// --touches`.
///
/// `projects` carries a slug column but is not in the entity-ref
/// resolver's table, so slug lookup for the `repo` kind has to be done by
/// hand — the same reason `handlers/plan.cpp` and `handlers/task.cpp` each
/// carry their own copy.
/// @param conn An open, migrated database connection.
/// @param slug The repo slug.
/// @return The row id, `std::nullopt` when no such project exists, or the
/// refusal when the query itself failed.
auto resolve_repo_slug_for_touches(db::connection& conn, std::string_view slug)
    -> std::expected<std::optional<std::int64_t>, domain_error> {
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

/// @brief The vendor identity for the session the create hook attaches to.
///
/// Unset AND empty both yield `cli` — the Zig original's semantics, not
/// "unset -> default". Same discipline as `handlers/unlink.cpp`'s copy.
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

/// @brief Write the created question through the requested renderer.
///
/// Per-renderer terminator contract: `render_text` carries its own trailing
/// newline, `render_json` is a fragment this caller terminates.
/// @param ctx The invocation context.
/// @param args The parsed arguments (read for `--json`).
/// @param q The question to render.
void emit(context& ctx, const cliapp::parsed_args& args, const pl::question& q) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(q) << '\n';
  } else {
    ctx.out() << pl::render_text(q);
  }
}

} // namespace

auto question_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation — zig's handler opens with
  // `try runtime.ensureDb()`, so even a refused invocation leaves a
  // created-and-migrated database behind. Same ordering rule every leaf in
  // this binary follows.
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  if (cliapp::flag_bool(args, "--editor")) {
    // Oracle-captured verbatim, including that it is a WARNING on stderr
    // and the create proceeds. Not a refusal: `--editor` is declared on the
    // leaf and the oracle honours the rest of the invocation.
    ctx.err() << "warning: --editor not yet implemented; falling back to inline create\n";
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};
  // `map_scope_error` composes `<verb>: resolving scope failed: <Tag>`, so
  // the verb passed here is the bare verb name — zig's add.zig dies with
  // exactly `"question add: resolving scope failed: {s}"`.
  auto resolved = resolve_write_scope(ctx, scope_view, "question add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  // NO `project_unassociated` refusal here — see this module's header.
  // `resolved->scope` staying unset inside an unassociated project is the
  // CORRECT outcome and lets the engine write `scope_kind='global'`.

  // The session is resolved BEFORE the create and its creation is a
  // COMMITTED SIDE EFFECT even when the create then fails: `question add
  // --scope nope` exits 1 having written a `sessions` row and a
  // `create|session|N|start session vendor=cli` audit row. Oracle-captured
  // against a scratch root. `plan create` and `task add` have no such
  // effect — this ordering is question-specific and observable, so it is
  // reproduced rather than tidied into the success path.
  auto const session_id = session::ensure_active(**conn, vendor_from(ctx), vendor_session_id_from(ctx));

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required), but an absent title must never fall through to "" and
    // write an untitled row.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "question add: title is required"));
  }
  auto const title_text = *title;

  auto created = pl::create_question(**conn, pl::question_create_args{
                                                 .title   = title_text,
                                                 .body    = cliapp::flag_string(args, "--body"),
                                                 .scope   = resolved->scope,
                                                 .plan_id = cliapp::flag_int(args, "--plan"),
                                             });
  if (!created) {
    return std::unexpected(map_question_error(created.error(), "question add"));
  }

  // The entity-create activity hook, composed HERE because the engine
  // cannot reach `engine_runtime` from layer 2 — see question.cppm's
  // header and `record_entity_create_action`'s own doc comment. Runs after
  // the create has committed and swallows every failure, so it can only
  // add an `agent_actions` row, never change this verb's outcome. A
  // session with no live claim makes it a silent no-op, which is the
  // interactive-operator case.
  if (session_id) {
    activity::record_entity_create_action(**conn, *session_id, activity::action_entity_kind::question, created->id,
                                          std::format("created question: {}", title_text));
  }

  emit(ctx, args, *created);
  return {};
}

auto question_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "question-id", "question");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto found = pl::show_question(**conn, *id);
  if (!found) {
    return std::unexpected(map_lookup_error(found.error(), "question show", *id));
  }
  emit(ctx, args, *found);
  return {};
}

auto question_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::question_list_filter filter{};
  filter.plan_id = cliapp::flag_int(args, "--plan");

  // `--status` is COMMA-SEPARATED here (`--status open,wontfix`), matching
  // `plan list` rather than `task list`'s single value. Confirmed by
  // running it: the flag's declared type is a plain string on all three.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    for (auto const& tok : split_csv(*raw)) {
      auto const st = pl::question_status_from_text(tok);
      if (!st) {
        // Exit 1, not 2: zig dies with `error.InvalidStatus`, which has no
        // arm in `codeFor`. Oracle-captured (`--status bogus` -> exit 1).
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", tok)));
      }
      filter.statuses.push_back(*st);
    }
  }

  // Note the target: `filter.scopes`, never `filter.scope`. The oracle's
  // handler fills the ARRAY in both branches and leaves the singular field
  // unset; the engine ORs the two together, so the distinction is invisible
  // in the result — but keeping to the oracle's shape means a future
  // caller that DOES set both gets the oracle's disjunction order.
  if (auto const raw = cliapp::flag_string(args, "--scope"); raw.has_value()) {
    // An EXPLICIT `--scope` bypasses the read set entirely and goes
    // straight to the engine, which resolves each slug and reports its own
    // `SlugNotFound`.
    filter.scopes = split_csv(*raw);
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    filter.scopes = std::move(*slugs);
  }

  // `--touches <repo-slug>` selects a DIFFERENT query (the direct-scope /
  // touches-edge UNION), not a post-filter over the plain listing — the two
  // branches treat the scope predicate differently and a post-filter could
  // not reproduce that.
  //
  // The repo slug is resolved HERE, and an unknown slug REFUSES rather than
  // listing empty: `repo 'nosuchrepo' not found`, oracle-captured. Falling
  // through to an empty listing is the silent-filter defect this milestone
  // keeps closing.
  std::expected<std::vector<pl::question>, pl::question_error> rows;
  if (auto const slug = cliapp::flag_string(args, "--touches"); slug.has_value()) {
    auto repo_id = resolve_repo_slug_for_touches(**conn, *slug);
    if (!repo_id) {
      return std::unexpected(repo_id.error());
    }
    if (!repo_id->has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug)));
    }
    rows = pl::list_questions_touching(**conn, **repo_id, filter);
    if (!rows) {
      return std::unexpected(map_question_error(rows.error(), "question list --touches"));
    }
  } else {
    rows = pl::list_questions(**conn, filter);
    if (!rows) {
      return std::unexpected(map_question_error(rows.error(), "question list"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto question_answer(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "question-id", "question");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto const answer = cliapp::flag_string(args, "--answer");
  if (!answer.has_value()) {
    // Exit 2 (`invalid_input`), and it fires BEFORE the row is looked up —
    // `question answer 999` with no `--answer` reports this, not NotFound.
    // The sibling empty-value refusal below exits 1. Both captured.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--answer is required"));
  }

  auto answered = pl::answer_question(**conn, *id, *answer);
  if (!answered) {
    if (answered.error() == pl::question_error::answer_required) {
      // `--answer ""`: PRESENT but empty. Exit 1 — the engine's
      // `AnswerRequired` has no arm in `codeFor`. The wording differs from
      // the absent-flag case above and so does the code.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "--answer must be non-empty"));
    }
    return std::unexpected(map_lookup_error(answered.error(), "question answer", *id));
  }
  emit(ctx, args, *answered);
  return {};
}

auto question_wontfix(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "question-id", "question");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto const reason = cliapp::flag_string(args, "--reason");
  auto const view   = reason.has_value() ? std::optional<std::string_view>{*reason} : std::optional<std::string_view>{};

  auto marked = pl::wontfix_question(**conn, *id, view);
  if (!marked) {
    return std::unexpected(map_lookup_error(marked.error(), "question wontfix", *id));
  }
  emit(ctx, args, *marked);
  return {};
}

auto question_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow —
  // see handlers/links.cppm's header.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::question, "question-id", "question", "question_id",
                          "question link", false);
}

namespace {

/// @brief Declare every child of the `question` group, in catalog order.
/// @param question The `question` group node.
auto declare_question_children(CLI::App& question) -> void {
  question_cli::attach_add(question);

  question_cli::attach_edit(question);

  question_cli::attach_view(question);

  question_cli::attach_diff(question);

  question_cli::attach_review(question);

  question_cli::attach_answer(question);

  question_cli::attach_wontfix(question);

  question_cli::attach_list(question);

  question_cli::attach_show(question);

  question_cli::attach_link(question);
}

} // namespace

auto declare_question(CLI::App& root) -> void {
  CLI::App* question = root.add_subcommand("question", "Manage questions — open uncertainties surfaced during work.\n\n  Status "
                                                       "lifecycle: open → answered (via 'question answer') / wontfix.");
  question->require_subcommand(0);
  declare_question_children(*question);
}

} // namespace planar::cmd::handlers
