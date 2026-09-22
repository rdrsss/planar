/// @file scenario.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.scenario`.

module planar.cmd.planar.handlers.scenario;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.engine.entitylink;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.scenario.add;
import planar.cmd.planar.handlers.scenario.edit;
import planar.cmd.planar.handlers.scenario.view;
import planar.cmd.planar.handlers.scenario.diff;
import planar.cmd.planar.handlers.scenario.review;
import planar.cmd.planar.handlers.scenario.verify;
import planar.cmd.planar.handlers.scenario.retire;
import planar.cmd.planar.handlers.scenario.list;
import planar.cmd.planar.handlers.scenario.show;
import planar.cmd.planar.handlers.scenario.link;

namespace planar::cmd::handlers {

namespace pl = engine::planning;

namespace {

/// @brief The Zig error name for a `scenario_error`.
///
/// zig's handlers die with `exit.die(ctx, e, "scenario add: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. `SlugNotFound`, `QueryFailed` and
/// `IllegalTransition` were captured from the oracle directly (`scenario
/// add --scope nosuchslug`, `scenario add --related 77`, `scenario verify`
/// on a retired row); the rest are transcribed from
/// zig/src/engine/planning/scenario.zig's error set.
///
/// Note `IllegalTransition` reaches the operator UNFOLDED — the scenario
/// engine `try`s `policy.status.check` straight out rather than rewriting
/// it the way `decision` rewrites the same failure into `is terminal;
/// cannot accept`. So the scenario family has no humanised transition
/// refusal at all, which is the oracle's shape and not an omission here.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::scenario_error err) -> std::string_view {
  switch (err) {
  case pl::scenario_error::not_found:
    return "NotFound";
  case pl::scenario_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::scenario_error::slug_not_found:
    return "SlugNotFound";
  case pl::scenario_error::illegal_transition:
    return "IllegalTransition";
  case pl::scenario_error::unknown_status:
    return "UnknownStatus";
  case pl::scenario_error::query_failed:
    return "QueryFailed";
  case pl::scenario_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map a `scenario_error` onto this binary's exit-code bucket.
///
/// EVERY member lands in the generic bucket (exit 1). None of the Zig tags
/// this family raises has an arm in `zig/src/cmd/planar/exit.zig`'s
/// `codeFor` — not `SlugNotFound`, not `IllegalTransition`. Verified by
/// running each against the oracle rather than read off the table. The two
/// refusals that DO exit 2 (`unknown outcome '<tok>'`, the id-parse
/// failure) come from the handler's own `invalid_input`, not from here.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"scenario add"`.
/// @return The mapped failure.
auto map_scenario_error(pl::scenario_error err, std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief Map an engine error for a single-id verb, giving `not_found` the
/// oracle's own dedicated message instead of the generic shape.
/// @param err The engine error.
/// @param verb The verb name to lead a generic message with.
/// @param id The scenario id, interpolated into the `not_found` message.
/// @return The mapped failure.
auto map_lookup_error(pl::scenario_error err, std::string_view verb, std::int64_t id) -> domain_error {
  if (err == pl::scenario_error::not_found) {
    // ORACLE: `error: no scenario with id 999`, NOT `scenario show:
    // NotFound`. Captured from show/verify/retire alike.
    return error_from_body(domain_error_kind::not_found, std::format("no scenario with id {}", id));
  }
  return map_scenario_error(err, verb);
}

/// @brief Split a comma-separated flag value into trimmed, non-empty
/// tokens.
///
/// A fourth copy of the four-line rule `handlers/plan.cpp`,
/// `handlers/task.cpp` and `handlers/question.cpp` each carry, kept local
/// for the same reason they are: the units are separate and this is
/// vocabulary, not policy.
///
/// The EMPTY-token skip is load-bearing, not tidiness: `--status ""` and
/// `--scope ""` therefore yield NO tokens, which for both flags means "no
/// predicate" and lists everything. Oracle-captured for both.
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

/// @brief Resolve a repo slug to a `projects.id` for `scenario list
/// --touches`.
///
/// `projects` carries a slug column but is not in the entity-ref
/// resolver's table, so slug lookup for the `repo` kind has to be done by
/// hand — the same reason `handlers/question.cpp` carries its own copy.
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

/// @brief Write one scenario through the requested renderer.
///
/// Per-renderer terminator contract: `render_text` carries its own trailing
/// newline, `render_json` is a fragment this caller terminates.
/// @param ctx The invocation context.
/// @param args The parsed arguments (read for `--json`).
/// @param s The scenario to render.
void emit(context& ctx, const cliapp::parsed_args& args, const pl::scenario& s) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(s) << '\n';
  } else {
    ctx.out() << pl::render_text(s);
  }
}

} // namespace

auto scenario_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
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
  // exactly `"scenario add: resolving scope failed: {s}"`.
  auto resolved = resolve_write_scope(ctx, scope_view, "scenario add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  // NO `project_unassociated` refusal — `resolved->scope` staying unset
  // inside an unassociated project is the CORRECT outcome and lets the
  // engine write `scope_kind='global'`.

  if (cliapp::flag_bool(args, "--editor")) {
    // ORACLE, verbatim, on stderr, and then it PROCEEDS. Unlike `decision
    // add --editor`, this is not a compensating message for an unported
    // flow — the Zig verb prints this same line and falls through to the
    // same inline create. Reproducing it keeps stderr byte-identical too.
    ctx.err() << "warning: --editor not yet implemented; falling back to inline create\n";
  }

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required), but an absent title must never fall through to "" and
    // write an untitled row.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "scenario add: title is required"));
  }

  auto created = pl::create_scenario(**conn, pl::scenario_create_args{
                                                 .title               = *title,
                                                 .body                = cliapp::flag_string(args, "--body"),
                                                 .related_artifact_id = cliapp::flag_int(args, "--related"),
                                                 .plan_id             = cliapp::flag_int(args, "--plan"),
                                                 .scope               = resolved->scope,
                                             });
  if (!created) {
    // A nonexistent `--related` artifact arrives here as `QueryFailed`,
    // from the column's foreign key. That is the oracle's answer too and it
    // is deliberately NOT humanised into a "no artifact with id N" — doing
    // so would invent a refusal the oracle does not have.
    return std::unexpected(map_scenario_error(created.error(), "scenario add"));
  }

  // No entity-create activity hook and no session, unlike `question add` /
  // `decision add`: zig's scenario/add.zig calls neither. See
  // scenario.cppm's header.

  emit(ctx, args, *created);
  return {};
}

auto scenario_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "scenario-id", "scenario");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto found = pl::show_scenario(**conn, *id);
  if (!found) {
    return std::unexpected(map_lookup_error(found.error(), "scenario show", *id));
  }
  emit(ctx, args, *found);
  return {};
}

auto scenario_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::scenario_list_filter filter{};
  filter.related_artifact_id = cliapp::flag_int(args, "--related");

  // `--status` is COMMA-SEPARATED here, matching `question list` and `plan
  // list` and NOT `decision list`. An EMPTY result (no flag, or `--status
  // ""`) means EVERY status — see scenario.cppm.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    for (auto const& tok : split_csv(*raw)) {
      auto const st = pl::scenario_status_from_text(tok);
      if (!st) {
        // Exit 1, not 2: zig dies with `error.InvalidStatus`, which has no
        // arm in `codeFor`. The identical refusal on `decision list` exits
        // 2. Both captured.
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", tok)));
      }
      filter.statuses.push_back(*st);
    }
  }

  // Note the target: `filter.scopes`, never `filter.scope`. The oracle's
  // handler fills the ARRAY in both branches and leaves the singular field
  // unset — the mirror image of what `decision list`'s handler does.
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
  // arms treat the scope predicate differently and a post-filter could not
  // reproduce that. Unlike `plan list` / `task list`, this leaf SERVES the
  // flag: `list_scenarios_touching` is ported.
  //
  // The repo slug is resolved HERE, and an unknown slug REFUSES rather than
  // listing empty: `repo 'nosuchrepo' not found`, oracle-captured.
  std::expected<std::vector<pl::scenario>, pl::scenario_error> rows;
  if (auto const slug = cliapp::flag_string(args, "--touches"); slug.has_value()) {
    auto repo_id = resolve_repo_slug_for_touches(**conn, *slug);
    if (!repo_id) {
      return std::unexpected(repo_id.error());
    }
    if (!repo_id->has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug)));
    }
    rows = pl::list_scenarios_touching(**conn, **repo_id, filter);
    if (!rows) {
      return std::unexpected(map_scenario_error(rows.error(), "scenario list --touches"));
    }
  } else {
    rows = pl::list_scenarios(**conn, filter);
    if (!rows) {
      return std::unexpected(map_scenario_error(rows.error(), "scenario list"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto scenario_verify(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "scenario-id", "scenario");
  if (!id) {
    return std::unexpected(id.error());
  }

  // `--outcome` DEFAULTS to `pass`, which is what makes the bare verb the
  // "this passed" transition. The default is applied HERE and not in the
  // surface's `default_value`, matching zig's `args.outcome orelse "pass"`.
  auto const raw     = cliapp::flag_string(args, "--outcome");
  auto const text    = raw.has_value() ? std::string_view{*raw} : std::string_view{"pass"};
  auto const outcome = pl::scenario_outcome_from_text(text);
  if (!outcome) {
    // Exit 2 — zig raises `error.InvalidInput` here, unlike the `--status`
    // refusal on the sibling `list` leaf, which exits 1.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unknown outcome '{}' (want pass|fail|error|skipped)", text)));
  }

  auto const summary_flag = cliapp::flag_string(args, "--summary");
  auto const summary =
      summary_flag.has_value() ? std::optional<std::string_view>{*summary_flag} : std::optional<std::string_view>{};

  auto verified = pl::verify_scenario(**conn, *id, *outcome, summary);
  if (!verified) {
    return std::unexpected(map_lookup_error(verified.error(), "scenario verify", *id));
  }
  emit(ctx, args, *verified);
  return {};
}

auto scenario_retire(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "scenario-id", "scenario");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto const reason_flag = cliapp::flag_string(args, "--reason");
  auto const reason = reason_flag.has_value() ? std::optional<std::string_view>{*reason_flag} : std::optional<std::string_view>{};

  auto retired = pl::retire_scenario(**conn, *id, reason);
  if (!retired) {
    return std::unexpected(map_lookup_error(retired.error(), "scenario retire", *id));
  }
  emit(ctx, args, *retired);
  return {};
}

auto scenario_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow —
  // see handlers/links.cppm's header. The subject kind is `test_scenario`,
  // which is what appears in the rendered line and in `entity_links`.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::test_scenario, "scenario-id", "scenario", "scenario_id",
                          "scenario link", false);
}

namespace {

/// @brief Declare every child of the `scenario` group, in catalog order.
/// @param scenario The `scenario` group node.
auto declare_scenario_children(CLI::App& scenario) -> void {
  scenario_cli::attach_add(scenario);

  scenario_cli::attach_edit(scenario);

  scenario_cli::attach_view(scenario);

  scenario_cli::attach_diff(scenario);

  scenario_cli::attach_review(scenario);

  scenario_cli::attach_verify(scenario);

  scenario_cli::attach_retire(scenario);

  scenario_cli::attach_list(scenario);

  scenario_cli::attach_show(scenario);

  scenario_cli::attach_link(scenario);
}

} // namespace

auto declare_scenario(CLI::App& root) -> void {
  CLI::App* scenario = root.add_subcommand(
      "scenario",
      "Manage test scenarios — verification artifacts tied to specs,\n  plans, or tasks.\n\n  Planar records scenarios and their "
      "outcomes; it does not execute\n  them.\n  Status lifecycle: draft → ready → verified / failing → retired.\n  Transitions: "
      "`scenario verify` (draft→verified via auto-ready, or ready→verified),\n  `scenario retire` (any→retired).");
  scenario->require_subcommand(0);
  declare_scenario_children(*scenario);
}

} // namespace planar::cmd::handlers
