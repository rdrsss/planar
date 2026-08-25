/// @file plan.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.plan`.

module planar.cmd.planar.handlers.plan;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
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

auto plan_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
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
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  if (cliapp::flag_string(args, "--touches").has_value()) {
    return std::unexpected(touches_not_implemented("plan list"));
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

  auto rows = pl::list_plans(**conn, filter);
  if (!rows) {
    return std::unexpected(map_plan_error_for(rows.error(), "plan list"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto plan_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
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
  auto conn = ctx.ensure_db();
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

} // namespace planar::cmd::handlers
