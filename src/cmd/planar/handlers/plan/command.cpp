/// @file plan.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.plan`.

module planar.cmd.planar.handlers.plan;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.identity;
import planar.engine.planning;
import planar.engine.ingest;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.engine.entitylink;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.plan.step_add;
import planar.cmd.planar.handlers.plan.step_list;
import planar.cmd.planar.handlers.plan.step_done;
import planar.cmd.planar.handlers.plan.step_skip;
import planar.cmd.planar.handlers.plan.step_link;
import planar.cmd.planar.handlers.plan.create;
import planar.cmd.planar.handlers.plan.show;
import planar.cmd.planar.handlers.plan.list;
import planar.cmd.planar.handlers.plan.update;
import planar.cmd.planar.handlers.plan.edit;
import planar.cmd.planar.handlers.plan.view;
import planar.cmd.planar.handlers.plan.diff;
import planar.cmd.planar.handlers.plan.review;
import planar.cmd.planar.handlers.plan.link;
import planar.cmd.planar.handlers.plan.next;
import planar.cmd.planar.handlers.plan.recommend_strategy;
import planar.cmd.planar.handlers.plan.divergence;
import planar.cmd.planar.handlers.plan.recompute_status;
import planar.cmd.planar.handlers.plan.closeout;
import planar.cmd.planar.handlers.plan.step;
import planar.cmd.planar.handlers.plan.descendants;

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
  case pl::plan_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
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

/// @brief `map_plan_error` for a verb other than `plan create`, which leads
/// its message with its own verb name.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"plan update"`.
/// @return The mapped failure.
auto map_plan_error_for(pl::plan_error err, std::string_view verb) -> domain_error {
  auto const kind = err == pl::plan_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief Split a comma-separated flag value the way the oracle's
/// `plan list --status` / `--scope` do: split on `,`, trim ASCII spaces
/// from each token, DROP empty tokens.
///
/// Dropping empties rather than passing them through is the oracle's
/// behaviour and it matters: `--scope " global , project:x "` resolves both
/// members, and a trailing comma does not produce an empty-slug lookup. That
/// is the same trailing-separator shape that wrote `slug='_'` at task 6133's
/// sibling defect, arrived at from the other direction.
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

/// @brief Resolve a repo slug to a `projects.id` for `plan list --touches`.
///
/// A direct query: `projects` carries a slug column but is not in the
/// entity-ref resolver's table, so slug lookup for the `repo` kind has to
/// be done by hand. `handlers/task.cpp` carries its own copy for the same
/// reason, and so does each of the oracle's handlers — the two live in
/// different translation units and neither is layer-1 vocabulary worth
/// extracting for two call sites.
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

/// @brief Emit the oracle's stderr advisory when closing a plan that still
/// has open descendant plans.
///
/// Best-effort and NEVER a refusal, exactly as the oracle has it: closing a
/// parent whose remaining milestones are moot is legitimate, and only the
/// operator can tell that from an accident. Any query failure is silent —
/// a diagnostic must not turn a valid status update into an error.
///
/// The recursion matters. `plan closeout` gates on ALL descendants, not
/// direct children, and this advisory uses the same definition so the two
/// agree about what "still open" means.
/// @param ctx The invocation context (the advisory goes to `ctx.err()`).
/// @param conn An open, migrated database connection.
/// @param plan_id The plan being closed.
void warn_open_descendants(context& ctx, db::connection& conn, std::int64_t plan_id) {
  auto stmt = conn.prepare("with recursive desc_plans(id) as ("
                           "  select id from plans where parent_plan_id = ?"
                           "  union all"
                           "  select p.id from plans p join desc_plans dp on p.parent_plan_id = dp.id"
                           ") select count(*) from plans where id in (select id from desc_plans)"
                           "  and status in ('draft','active','paused')");
  if (!stmt) {
    return;
  }
  if (auto b = stmt->bind_int64(1, plan_id); !b) {
    return;
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return;
  }
  auto const open = stmt->column_int64(0);
  if (open == 0) {
    return;
  }
  ctx.err() << std::format("warning: plan {} still has {} open descendant plan(s); closing it here does not close "
                           "them, and nothing will resurface them\n"
                           "         `planar plan closeout {}` gates on descendants and reports what blocks\n",
                           plan_id, open, plan_id);
}

/// @brief Whether the task's authoritative routing packet is dispatchable.
///
/// `plan next` calls this for every available task and therefore blocks an
/// absent packet. `recommend-strategy` first checks for materialized routing
/// provenance, retaining planning-time recommendations for tasks not yet
/// ingested; after provenance exists, it calls this same verdict.
/// @param conn An open database connection.
/// @param task_id The task to inspect.
/// @param verb The caller's verb for its query-failure diagnostic.
/// @return Whether the assembled packet is ready.
auto routing_packet_ready(db::connection& conn, std::int64_t task_id, std::string_view verb)
    -> std::expected<bool, domain_error> {
  auto packet = engine::ingest::packet::assemble_task(conn, task_id);
  if (!packet) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("{} packet: QueryFailed", verb)));
  }
  return packet->ready();
}

/// @brief Whether ingest has materialized routing provenance for a task.
auto has_routing_provenance(db::connection& conn, std::int64_t task_id, std::string_view verb)
    -> std::expected<bool, domain_error> {
  auto fact = conn.prepare("select 1 from routing_task_facts where task_id = ? limit 1");
  if (!fact || !fact->bind_int64(1, task_id)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("{} packet: QueryFailed", verb)));
  }
  auto stepped = fact->step();
  if (!stepped) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("{} packet: QueryFailed", verb)));
  }
  if (*stepped == db::step_result::done) {
    return false;
  }
  return true;
}

} // namespace

auto plan_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation. zig's handler opens with `const
  // d = try runtime.ensureDb();` and only then parses `--status`, so a
  // refused `--status bogus` still leaves a created-and-migrated database
  // behind. Oracle-confirmed against an empty scratch root: exit 1, and a
  // 1179648-byte `planar.db` on disk afterwards. Validating first would be
  // tidier and would silently change when the schema gets applied.
  auto conn = ctx.db().ensure_db();
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

auto plan_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto found = pl::show_plan(**conn, *id);
  if (!found) {
    if (found.error() == pl::plan_error::not_found) {
      // The oracle's `NotFound` arm has its OWN message ("no plan with id
      // 5"), not the generic `plan show: NotFound` shape. Oracle-captured.
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *id)));
    }
    return std::unexpected(map_plan_error_for(found.error(), "plan show"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(*found) << '\n';
  } else {
    ctx.out() << pl::render_text(*found);
  }
  return {};
}

auto plan_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::plan_list_filter filter{};
  filter.parent_plan_id = cliapp::flag_int(args, "--parent");

  // `--status` on THIS verb is a comma-separated LIST (`--status
  // draft,active`); the sibling `task list --status` is a single value.
  // The asymmetry is the oracle's — `plan/list.zig` splits, `task/list.zig`
  // does not — and it was confirmed by running both.
  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    for (auto const& tok : split_csv(*raw)) {
      auto const st = pl::plan_status_from_text(tok);
      if (!st) {
        // Exit 1, not 2: zig dies with `error.InvalidStatus`, which has no
        // arm in `codeFor`. Oracle-captured (`--status bogus` -> exit 1).
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", tok)));
      }
      filter.statuses.push_back(*st);
    }
  }

  if (auto const raw = cliapp::flag_string(args, "--scope"); raw.has_value()) {
    // An EXPLICIT `--scope` bypasses the read set entirely and goes straight
    // to the engine, which resolves each slug and reports its own
    // `SlugNotFound`. Comma-separated here too, and trimmed — `--scope
    // " global , project:x "` resolves both, oracle-confirmed.
    filter.scopes = split_csv(*raw);
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    filter.scopes = std::move(*slugs);
  }

  // `--touches <repo-slug>` selects a DIFFERENT query (the direct-scope /
  // touches-edge UNION), not a post-filter over the plain listing — the
  // two branches treat the scope predicate differently and a post-filter
  // could not reproduce that. Refused at exit 64 until task 6187.
  //
  // The repo slug is resolved HERE, and an unknown slug refuses rather
  // than listing empty: `repo 'nosuchrepo' not found`, oracle-captured.
  // Falling through to an empty listing is the silent-filter defect this
  // whole milestone keeps closing.
  std::expected<std::vector<pl::plan>, pl::plan_error> rows;
  if (auto const slug = cliapp::flag_string(args, "--touches"); slug.has_value()) {
    auto repo_id = resolve_repo_slug_for_touches(**conn, *slug);
    if (!repo_id) {
      return std::unexpected(repo_id.error());
    }
    if (!repo_id->has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo '{}' not found", *slug)));
    }
    rows = pl::list_plans_touching(**conn, **repo_id, filter);
    if (!rows) {
      return std::unexpected(map_plan_error_for(rows.error(), "plan list --touches"));
    }
  } else {
    rows = pl::list_plans(**conn, filter);
    if (!rows) {
      return std::unexpected(map_plan_error_for(rows.error(), "plan list"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto plan_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  pl::plan_update_args patch{};
  patch.title   = cliapp::flag_string(args, "--title");
  patch.slug    = cliapp::flag_string(args, "--slug");
  patch.summary = cliapp::flag_string(args, "--summary");
  patch.scope   = cliapp::flag_string(args, "--scope");

  // `--parent 0` is the CLEAR sentinel, not plan id 0. Threading the raw
  // value straight into `parent_plan_id` would write a dangling foreign key
  // and — because `plans.parent_plan_id` has no row 0 — surface as a
  // QueryFailed rather than as the clear the operator asked for.
  if (auto const parent = cliapp::flag_int(args, "--parent"); parent.has_value()) {
    if (*parent == 0) {
      patch.clear_parent = true;
    } else {
      patch.parent_plan_id = *parent;
    }
  }

  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::plan_status_from_text(*raw);
    if (!st) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
    }
    patch.status = *st;
  }

  // Advisory, never a refusal — and suppressed under `--json`, because the
  // oracle gates it on `!args.json` so a machine-readable run stays clean.
  if (patch.status.has_value() && (*patch.status == pl::plan_status::done || *patch.status == pl::plan_status::abandoned) &&
      !cliapp::flag_bool(args, "--json")) {
    warn_open_descendants(ctx, **conn, *id);
  }

  auto updated = pl::update_plan(**conn, *id, patch);
  if (!updated) {
    if (updated.error() == pl::plan_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *id)));
    }
    return std::unexpected(map_plan_error_for(updated.error(), "plan update"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(*updated) << '\n';
  } else {
    ctx.out() << pl::render_text(*updated);
  }
  return {};
}

auto plan_recompute_status(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const plan_flag = cliapp::flag_int(args, "--plan");
  auto const all_flag  = cliapp::flag_bool(args, "--all");
  auto const as_json   = cliapp::flag_bool(args, "--json");

  // Exactly one. Both refusals are exit 2 (`error.InvalidInput`), captured.
  if (!plan_flag.has_value() && !all_flag) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "either --plan <id> or --all is required"));
  }
  if (plan_flag.has_value() && all_flag) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--plan and --all are mutually exclusive"));
  }

  auto const emit_one = [&](const pl::recompute_result& r) {
    if (as_json) {
      ctx.out() << std::format(R"({{"plan_id":{},"status_before":"{}","status_after":"{}","flipped":{}}})", r.plan_id,
                               pl::plan_status_to_text(r.status_before), pl::plan_status_to_text(r.status_after),
                               r.flipped ? "true" : "false")
                << '\n';
    } else if (r.flipped) {
      // U+2192 RIGHTWARDS ARROW, not "->" — the oracle prints the arrow
      // character. Captured from a flipped run.
      ctx.out() << std::format("plan {}: {} \xe2\x86\x92 {}\n", r.plan_id, pl::plan_status_to_text(r.status_before),
                               pl::plan_status_to_text(r.status_after));
    }
  };

  if (!all_flag) {
    auto result = pl::recompute_status(**conn, *plan_flag);
    if (!result) {
      if (result.error() == pl::plan_error::not_found) {
        return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *plan_flag)));
      }
      return std::unexpected(map_plan_error_for(result.error(), "recompute-status"));
    }
    // Single-plan mode prints the unflipped case too; the `--all` walk does
    // NOT (it prints only transitions, then a tally). Both captured.
    if (!result->flipped && !as_json) {
      ctx.out() << std::format("plan {}: {} (no change)\n", result->plan_id, pl::plan_status_to_text(result->status_before));
    } else {
      emit_one(*result);
    }
    return {};
  }

  // `--all` walks every plan the DEFAULT filter returns, and the default
  // filter is not "everything": an empty `statuses` means the three OPEN
  // statuses, so terminal (done/abandoned) plans are NOT recomputed. That
  // is load-bearing rather than incidental — including them would let the
  // aggregate matrix flip a `done` plan back to `active` on any child that
  // is not terminal, which is a resurrection, not a recompute. (The C++
  // engine did exactly that until this cycle; see `list_plans`.)
  //
  // What the default filter deliberately does NOT carry is a scope: the
  // walk is not cwd-scoped, so passing the read set here would silently
  // recompute only the plans visible from this directory.
  auto rows = pl::list_plans(**conn, pl::plan_list_filter{});
  if (!rows) {
    return std::unexpected(map_plan_error_for(rows.error(), "listing plans"));
  }

  std::size_t changed = 0;
  for (auto const& p : *rows) {
    auto result = pl::recompute_status(**conn, p.id);
    if (!result) {
      // Report and CONTINUE — one bad plan must not abandon the sweep.
      ctx.err() << std::format("plan {}: error: {}\n", p.id, zig_error_name(result.error()));
      continue;
    }
    if (result->flipped) {
      ++changed;
    }
    emit_one(*result);
  }
  if (!as_json) {
    // Leading blank line is the oracle's, and it is printed even when the
    // walk emitted nothing at all. Captured against a single-plan DB with
    // no transitions.
    ctx.out() << std::format("\nrecomputed {} plans; {} transitioned\n", rows->size(), changed);
  }
  return {};
}

// =========================================================================
// `plan step` — the five leaves landed at task 6187 alongside their engine.
//
// Every one of them phrases the engine's errors ITSELF rather than falling
// through to `map_plan_error_for`'s `<verb>: <ZigTag>` shape, because the
// oracle does: `no plan with id 999`, `ordinal conflict: ...`, `no step
// with id 99`, `step N is already terminal`, `step N cannot be skipped
// (must be pending)`, `step N or task M not found`. Six distinct strings,
// all exit 1, so nothing but a byte assertion separates them.
// =========================================================================

namespace {

/// @brief Parse a required integer positional using the `plan step`
/// family's OWN error wording.
///
/// `entity_id_arg` renders `"{label} id must be an integer, got '{raw}'"`,
/// which is right for `plan show` (`plan id must be ...`) and WRONG here:
/// every `plan step` leaf names the positional verbatim and appends no
/// noun, so the oracle says `plan-id must be an integer, got 'abc'` and
/// `step-id must be an integer, got 'abc'`. Passing `"plan-id"` as the
/// label to the shared helper produced `plan-id id must be ...` — caught
/// by the oracle-differential run, invisible to the exit code (both 2).
/// @param args The parsed arguments.
/// @param name The positional's declared name, used verbatim in the message.
/// @return The parsed id, or `invalid_input` (exit 2).
auto step_positional_id(const cliapp::parsed_args& args, std::string_view name) -> std::expected<std::int64_t, domain_error> {
  auto const raw = cliapp::positional_string(args, name);
  if (!raw.has_value()) {
    // Unreachable through the tree (all four are declared required), but an
    // absent id must never fall through to 0 and act on row 0.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("{} is required", name)));
  }
  auto const parsed = cliapp::parse_int64_zig(*raw);
  if (!parsed.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} must be an integer, got '{}'", name, *raw)));
  }
  return *parsed;
}

/// @brief Emit one step in whichever shape `--json` selects.
///
/// The `ok` sentinel rides on the JSON form for all four MUTATION leaves
/// and on none of the objects inside `plan step list`'s array — the
/// oracle's asymmetry, carried by `render_step_json`'s `with_ok`.
/// @param ctx The invocation context.
/// @param args The parsed arguments, read for `--json`.
/// @param s The step to render.
void emit_step(context& ctx, const cliapp::parsed_args& args, const pl::plan_step& s) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_step_json(s, true) << '\n';
  } else {
    ctx.out() << pl::render_step_text(s);
  }
}

/// @brief Map a `plan_step_error` that the caller has no bespoke message
/// for onto the generic `<verb>: <ZigTag>` shape.
///
/// Reached only by `query_failed` and `audit_write_failed` in practice;
/// every other member has a caller-specific string. Kept rather than
/// collapsed into a bare "failed" so an operator who hits the SQLite arm
/// still gets the tag the oracle would have printed.
/// @param err The engine error.
/// @param verb The verb name to lead with.
/// @return The mapped failure.
auto map_step_error(pl::plan_step_error err, std::string_view verb) -> domain_error {
  std::string_view tag = "Unknown";
  switch (err) {
  case pl::plan_step_error::not_found:
    tag = "NotFound";
    break;
  case pl::plan_step_error::invalid_transition:
    tag = "InvalidTransition";
    break;
  case pl::plan_step_error::ordinal_conflict:
    tag = "OrdinalConflict";
    break;
  case pl::plan_step_error::query_failed:
    tag = "QueryFailed";
    break;
  case pl::plan_step_error::audit_write_failed:
    tag = "WriteFailed";
    break;
  }
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, tag));
}

} // namespace

auto plan_step_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const plan_id = step_positional_id(args, "plan-id");
  if (!plan_id) {
    return std::unexpected(plan_id.error());
  }
  auto const body = cliapp::positional_string(args, "body");
  if (!body.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "body is required"));
  }

  pl::plan_step_add_args add{.plan_id = *plan_id, .body = *body, .ordinal = cliapp::flag_int(args, "--after")};
  auto                   created = pl::add_step(**conn, add);
  if (!created) {
    switch (created.error()) {
    case pl::plan_step_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("no plan with id {}", *plan_id)));
    case pl::plan_step_error::ordinal_conflict:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "ordinal conflict: a step at that position already exists"));
    default:
      return std::unexpected(map_step_error(created.error(), "plan step add"));
    }
  }
  emit_step(ctx, args, *created);
  return {};
}

auto plan_step_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const plan_id = step_positional_id(args, "plan-id");
  if (!plan_id) {
    return std::unexpected(plan_id.error());
  }

  auto rows = pl::list_steps(**conn, *plan_id);
  if (!rows) {
    return std::unexpected(map_step_error(rows.error(), "plan step list"));
  }
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_step_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_step_list_text(*rows, *plan_id);
  }
  return {};
}

namespace {

/// @brief The shared body of `plan step done` and `plan step skip`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param to_done Whether this is `done` (else `skip`).
/// @return The handler result.
auto step_transition_leaf(context& ctx, const cliapp::parsed_args& args, bool to_done) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const step_id = step_positional_id(args, "step-id");
  if (!step_id) {
    return std::unexpected(step_id.error());
  }

  auto moved = to_done ? pl::mark_step_done(**conn, *step_id) : pl::skip_step(**conn, *step_id);
  if (!moved) {
    switch (moved.error()) {
    case pl::plan_step_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("no step with id {}", *step_id)));
    case pl::plan_step_error::invalid_transition:
      // The two leaves phrase the SAME engine error differently, and
      // `skip` says "cannot be skipped" even for an already-terminal step
      // rather than borrowing `done`'s wording. Both oracle-captured
      // against the same row.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             to_done ? std::format("step {} is already terminal", *step_id)
                                                     : std::format("step {} cannot be skipped (must be pending)", *step_id)));
    default:
      return std::unexpected(map_step_error(moved.error(), to_done ? "plan step done" : "plan step skip"));
    }
  }
  emit_step(ctx, args, *moved);
  return {};
}

} // namespace

auto plan_step_done(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return step_transition_leaf(ctx, args, true);
}

auto plan_step_skip(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return step_transition_leaf(ctx, args, false);
}

auto plan_step_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const step_id = step_positional_id(args, "step-id");
  if (!step_id) {
    return std::unexpected(step_id.error());
  }
  auto const task_id = step_positional_id(args, "task-id");
  if (!task_id) {
    return std::unexpected(task_id.error());
  }

  auto linked = pl::link_step_task(**conn, *step_id, *task_id);
  if (!linked) {
    if (linked.error() == pl::plan_step_error::not_found) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("step {} or task {} not found", *step_id, *task_id)));
    }
    return std::unexpected(map_step_error(linked.error(), "plan step link"));
  }
  emit_step(ctx, args, *linked);
  return {};
}

auto plan_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `true`: `plan link` alone spells its duplicate-refusal arrow with
  // the UNICODE `\u2192`. Its two siblings use the ASCII `->`. Oracle
  // inconsistency, reproduced — see handlers/links.cppm's header.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::plan, "plan-id", "plan", "plan_id", "plan link", true);
}

auto plan_descendants(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace de = engine::planning::descendants;

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  // The handler's OWN existence probe, before the walk. See plan.cppm.
  {
    auto stmt = (*conn)->prepare("select count(*) from plans where id = ?");
    if (!stmt) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan descendants: QueryFailed"));
    }
    if (auto bound = stmt->bind_int64(1, *id); !bound) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan descendants: QueryFailed"));
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan descendants: QueryFailed"));
    }
    if (*stepped == db::step_result::done || stmt->column_int64(0) == 0) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *id)));
    }
  }

  auto const tree = de::walk_tree(**conn, *id);
  if (!tree) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("plan descendants: walk tree: {}",
                                    tree.error() == de::descendants_error::not_found ? "NotFound" : "QueryFailed")));
  }

  auto const kind_text = [](de::entry_kind kind) -> std::string_view { return kind == de::entry_kind::task ? "task" : "plan"; };
  auto const role_text = [](de::entry_kind kind) -> std::string_view {
    switch (kind) {
    case de::entry_kind::plan_anchor:
      return "anchor";
    case de::entry_kind::plan_child:
      return "child";
    case de::entry_kind::task:
      return "task";
    }
    return "task";
  };

  if (cliapp::flag_bool(args, "--json")) {
    std::string out   = "[";
    bool        first = true;
    for (auto const& entry : *tree) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += R"({"kind":)";
      json_text::append_json_string(out, kind_text(entry.kind));
      out += R"(,"role":)";
      json_text::append_json_string(out, role_text(entry.kind));
      out += std::format(R"(,"id":{},"title":)", entry.id);
      json_text::append_json_string(out, entry.title);
      out += "}";
    }
    out += "]\n";
    ctx.out() << out;
    return {};
  }

  std::string out;
  for (auto const& entry : *tree) {
    std::string_view prefix = "task";
    if (entry.kind == de::entry_kind::plan_anchor) {
      prefix = "plan (anchor)";
    } else if (entry.kind == de::entry_kind::plan_child) {
      prefix = "plan (child)";
    }
    out += std::format("{}:{}  {}\n", prefix, entry.id, entry.title);
  }
  ctx.out() << out;
  return {};
}

auto plan_next(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace aa = engine::runtime::agentactivity;
  namespace ar = engine::runtime::agentrender;

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  // The handler's own existence probe, exactly as `plan descendants` keeps
  // one: the selector returns an EMPTY result for an unknown plan rather
  // than failing, so without this probe `plan next 999` would print a
  // cheerful all-zeros header at exit 0 instead of refusing.
  {
    auto stmt = (*conn)->prepare("select count(*) from plans where id = ?");
    if (!stmt || !stmt->bind_int64(1, *id)) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan next: QueryFailed"));
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan next: QueryFailed"));
    }
    if (*stepped == db::step_result::done || stmt->column_int64(0) == 0) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("plan {} not found", *id)));
    }
  }

  auto rows = aa::next_work(**conn, *id);
  if (!rows) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("plan next selector: {}", aa::error_name(rows.error()))));
  }

  std::size_t n_avail   = 0;
  std::size_t n_claimed = 0;
  std::size_t n_stale   = 0;
  std::size_t n_blocked = 0;
  // A claim-aware task selector alone is insufficient for dispatch: a task
  // whose provenance packet is incomplete must remain blocked until ingest
  // reconciliation restores its reviewed evidence.
  for (auto& row : *rows) {
    if (row.bucket == aa::next_work_bucket::available) {
      auto ready = routing_packet_ready(**conn, row.task_id, "plan next");
      if (!ready) {
        return std::unexpected(ready.error());
      }
      if (!*ready)
        row.bucket = aa::next_work_bucket::blocked;
    }
    switch (row.bucket) {
    case aa::next_work_bucket::available:
      ++n_avail;
      break;
    case aa::next_work_bucket::claimed:
      ++n_claimed;
      break;
    case aa::next_work_bucket::stale:
      ++n_stale;
      break;
    case aa::next_work_bucket::blocked:
      ++n_blocked;
      break;
    }
  }

  // Its own query over the same `plan_tree` CTE — see plan.cppm.
  std::int64_t done_count = 0;
  {
    auto stmt = (*conn)->prepare("with recursive plan_tree(id) as (\n"
                                 "  select id from plans where id = ?\n"
                                 "  union all\n"
                                 "  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id\n"
                                 ")\n"
                                 "select count(*) from tasks\n"
                                 "where plan_id in (select id from plan_tree) and status = 'done'");
    if (!stmt || !stmt->bind_int64(1, *id)) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan next done count: QueryFailed"));
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan next done count: QueryFailed"));
    }
    if (*stepped == db::step_result::row) {
      done_count = stmt->column_int64(0);
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    // One pass per bucket, in the oracle's field order. The claimed and
    // stale arrays wrap each task in a `{task, claim}` envelope; available
    // and blocked are bare task objects.
    auto const append_task_for = [&](std::string& out, std::int64_t task_id) -> bool {
      auto row = aa::get_task(**conn, task_id);
      if (!row) {
        return false;
      }
      ar::append_task(out, *row);
      return true;
    };

    std::string out = std::format("{{\"plan_id\":{}", *id);
    for (auto const bucket : {aa::next_work_bucket::available, aa::next_work_bucket::claimed, aa::next_work_bucket::stale,
                              aa::next_work_bucket::blocked}) {
      std::string_view key = "available";
      if (bucket == aa::next_work_bucket::claimed) {
        key = "claimed";
      } else if (bucket == aa::next_work_bucket::stale) {
        key = "stale";
      } else if (bucket == aa::next_work_bucket::blocked) {
        key = "blocked";
      }
      bool const wrapped = bucket == aa::next_work_bucket::claimed || bucket == aa::next_work_bucket::stale;

      out += std::format(",\"{}\":[", key);
      bool first = true;
      for (auto const& row : *rows) {
        if (row.bucket != bucket) {
          continue;
        }
        if (!first) {
          out += ",";
        }
        first = false;
        if (wrapped) {
          out += R"({"task":)";
        }
        if (!append_task_for(out, row.task_id)) {
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan next: QueryFailed"));
        }
        if (wrapped) {
          out += R"(,"claim":)";
          if (row.covering.has_value()) {
            ar::append_claim_view(out, *row.covering,
                                  ar::claim_view_extras{.entity_scope = aa::resolve_claim_scope(**conn, *row.covering)});
          } else {
            out += "null";
          }
          out += "}";
        }
      }
      out += "]";
    }
    out += std::format(",\"summary\":{{\"available\":{},\"claimed\":{},\"stale\":{},\"blocked\":{},\"done\":{}}}}}\n", n_avail,
                       n_claimed, n_stale, n_blocked, done_count);
    ctx.out() << out;
    return {};
  }

  bool const include_claimed = cliapp::flag_bool(args, "--include-claimed");
  bool const include_stale   = cliapp::flag_bool(args, "--include-stale");

  std::string out = std::format("plan:{}  available:{}  claimed:{}  stale:{}  blocked:{}  done:{}\n", *id, n_avail, n_claimed,
                                n_stale, n_blocked, done_count);
  for (auto const& row : *rows) {
    // The token stand-in for a bucket row whose claim could not be read
    // back. Unreachable through the CLI (the id came from the same
    // transaction) but the oracle spells it, so it is spelled here.
    // Both arms are spelled as `string_view` deliberately. Written as a
    // ternary over `std::string` and a literal, the common type is
    // `std::string`, so the view would be built from a temporary that dies
    // at the end of the expression — which `-Wreturn-stack-address` caught
    // here rather than at runtime.
    auto const token = [&] -> std::string_view {
      if (!row.covering.has_value()) {
        return std::string_view{"?"};
      }
      return std::string_view{row.covering->claim_token};
    };
    switch (row.bucket) {
    case aa::next_work_bucket::available:
      out += std::format("  available  task:{}  {}  [pri:{}]\n", row.task_id, row.title, row.priority);
      break;
    case aa::next_work_bucket::claimed:
      if (include_claimed) {
        out += std::format("  claimed    task:{}  {}  [pri:{}, claim:{}]\n", row.task_id, row.title, row.priority, token());
      }
      break;
    case aa::next_work_bucket::stale:
      if (include_stale) {
        out += std::format("  stale      task:{}  {}  [pri:{}, claim:{}]\n", row.task_id, row.title, row.priority, token());
      }
      break;
    case aa::next_work_bucket::blocked:
      out += std::format("  blocked    task:{}  {}  [pri:{}]\n", row.task_id, row.title, row.priority);
      break;
    }
  }
  ctx.out() << out;
  return {};
}

namespace {

/// The shared not-found refusal for the two strategy leaves.
///
/// Both verbs emit `plan {id} not found` at exit 1 (`not_found` maps to
/// `exit_generic_failure`). Captured identically from both, in both `--json`
/// and text mode -- the refusal is NOT a JSON document in either.
auto strategy_plan_not_found(std::int64_t plan_id) -> domain_error {
  return error_from_body(domain_error_kind::not_found, std::format("plan {} not found", plan_id));
}

auto strategy_query_failed(std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: QueryFailed", verb));
}

} // namespace

auto plan_recommend_strategy(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace st = engine::planning::strategy;

  // Both refusals are argument-shaped and come BEFORE the database is
  // touched, matching the oracle: a bad `--closure-source` on a nonexistent
  // plan reports the flag, not the plan.
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto const raw_source = cliapp::flag_string(args, "--closure-source");
  // The flag is declared with a `declared` default, so an absent value here
  // means the parser handed us nothing -- treat it as the default rather than
  // refusing on an empty string the operator never typed.
  auto const source_text = raw_source.has_value() ? *raw_source : std::string{"declared"};
  auto const source      = st::parse_closure_source(source_text);
  if (!source.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input,
                        std::format("--closure-source must be 'declared' or 'derived', got '{}'", source_text)));
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto rec = st::recommend_with(**conn, *id, *source);
  if (!rec) {
    if (rec.error() == st::strategy_error::not_found) {
      return std::unexpected(strategy_plan_not_found(*id));
    }
    return std::unexpected(strategy_query_failed("recommend-strategy"));
  }

  std::vector<st::task_ref> packet_ready;
  packet_ready.reserve(rec->parallel_eligible.size());
  for (auto& task : rec->parallel_eligible) {
    auto provenance = has_routing_provenance(**conn, task.id, "recommend-strategy");
    if (!provenance) {
      return std::unexpected(provenance.error());
    }
    if (!*provenance) {
      packet_ready.push_back(std::move(task));
      continue;
    }
    auto ready = routing_packet_ready(**conn, task.id, "recommend-strategy");
    if (!ready) {
      return std::unexpected(ready.error());
    }
    if (*ready) {
      packet_ready.push_back(std::move(task));
      continue;
    }
    task.excluded_by.push_back(st::exclusion{.rule = 7, .reason = "excluded by rule 7: routing packet is not ready"});
    rec->serialized.push_back(std::move(task));
  }
  rec->parallel_eligible = std::move(packet_ready);
  rec->fan_out_available = rec->parallel_eligible.size() >= 2;

  ctx.out() << (cliapp::flag_bool(args, "--json") ? st::render_recommendation_json(*rec, *source)
                                                  : st::render_recommendation_text(*rec, *source));
  return {};
}

auto plan_divergence(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace st = engine::planning::strategy;

  // No `--closure-source` here: the CLI tree does not declare it for this
  // leaf, so passing it is `error: UnknownFlag` at exit 2 before the handler
  // runs. This verb reads BOTH sources by construction.
  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const div = st::compute_divergence(**conn, *id);
  if (!div) {
    if (div.error() == st::strategy_error::not_found) {
      return std::unexpected(strategy_plan_not_found(*id));
    }
    return std::unexpected(strategy_query_failed("plan divergence"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? st::render_divergence_json(*id, *div)
                                                  : st::render_divergence_text(*id, *div));
  return {};
}

auto plan_closeout(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  namespace co = engine::planning::closeout;

  auto const id = entity_id_arg(args, "plan-id", "plan");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  bool const dry_run = cliapp::flag_bool(args, "--dry-run");
  auto const result  = co::evaluate(**conn, *id, !dry_run, cliapp::flag_bool(args, "--check-merge"));
  if (!result) {
    switch (result.error()) {
    case co::closeout_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *id)));
    case co::closeout_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan closeout: QueryFailed"));
    case co::closeout_error::write_failed:
      // The apply transaction rolled back, so the plan is still open and
      // the audit log is still consistent with it.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan closeout: WriteFailed"));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "plan closeout: QueryFailed"));
  }

  // The report goes out FIRST and unconditionally — the refusal below is an
  // additional stderr line, not a replacement for it.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? co::render_json(*result) : co::render_text(*result, dry_run));

  // Only the APPLY path refuses. See this leaf's declaration in plan.cppm
  // for why `--dry-run` exits 0 here despite the help text saying otherwise.
  if (!result->ready && !dry_run) {
    return std::unexpected(
        error_from_body(domain_error_kind::sync_conflict,
                        std::format("plan {} is not ready to close ({} reason(s))", *id, result->blocked_by.size())));
  }
  return {};
}

// ---------------------------------------------------------------------------
// CLI DECLARATION (plan 1051, M11.3a, task 6631)
//
// The `plan` command tree, declared HERE rather than as `node_spec` data in
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
// already defaults to 0, so deleting it leaves a bare `planar plan`
// rendering its help page and exiting 0, and the catalog unchanged
// (break-probed at task 6631 iteration 1). The calls are kept as an
// explicit, defensive statement of intent, matching tree.cpp's root node
// and the planar-agent fold -- not because anything depends on them.
// ---------------------------------------------------------------------------

namespace {

/// @brief Declare the `plan step` subcommands.
///
/// Its own function only because the child names collide with `plan`'s
/// own (`add`/`list`/`link`); the call site below fixes where the group
/// itself sits among its siblings.
/// @param step The `plan step` group node.
auto declare_plan_step(CLI::App& step) -> void {
  plan_cli::attach_step_add(step);

  plan_cli::attach_step_list(step);

  plan_cli::attach_step_done(step);

  plan_cli::attach_step_skip(step);

  plan_cli::attach_step_link(step);
}

/// @brief Declare every child of the `plan` group, in catalog order.
/// @param plan The `plan` group node.
auto declare_plan_children(CLI::App& plan) -> void {
  plan_cli::attach_create(plan);

  plan_cli::attach_show(plan);

  plan_cli::attach_list(plan);

  plan_cli::attach_update(plan);

  plan_cli::attach_edit(plan);

  plan_cli::attach_view(plan);

  plan_cli::attach_diff(plan);

  plan_cli::attach_review(plan);

  plan_cli::attach_link(plan);

  plan_cli::attach_next(plan);

  plan_cli::attach_recommend_strategy(plan);

  plan_cli::attach_divergence(plan);

  plan_cli::attach_recompute_status(plan);

  plan_cli::attach_closeout(plan);

  CLI::App* step = plan_cli::attach_step(plan);
  declare_plan_step(*step);

  plan_cli::attach_descendants(plan);
}

} // namespace

auto declare_plan(CLI::App& root) -> void {
  CLI::App* plan =
      root.add_subcommand("plan", "Manage plans \xe2\x80\x94 the top-level structured intent for a body of work.\n\n  Plans may "
                                  "be hierarchical (--parent) and contain ordered steps\n  (plan step add).\n  Status lifecycle: "
                                  "draft \xe2\x86\x92 active \xe2\x86\x92 paused / done / abandoned.");
  plan->require_subcommand(0);
  declare_plan_children(*plan);
}

} // namespace planar::cmd::handlers
