/// @file workbench.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workbench`.

module planar.cmd.planar.handlers.workbench;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.workbench;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.editor;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace wb = engine::workbench;

namespace {

using kind_t = domain_error_kind;

/// @brief `--json` was passed.
auto wants_json(const cliapp::parsed_args& args) -> bool {
  return flag_bool(args, "--json");
}

/// @brief Read a positional that the tree declares as optional.
auto optional_positional(const cliapp::parsed_args& args, std::string_view name) -> std::optional<std::string> {
  return positional_string(args, name);
}

/// @brief Resolve the workbench root from the context's environment.
///
/// `create` mirrors the Zig split: `resolveAndEnsureWorkbenchRoot` (list,
/// gc, archive, restore) versus a plain `resolveRoot` (status, lint). See
/// this leaf module's header.
auto resolve_root(context& ctx, bool create) -> std::expected<std::string, domain_error> {
  auto root = wb::root::resolve_root(ctx.env());
  if (!root) {
    // `WorkbenchRootUnresolved` is the Zig error tag the oracle interpolates.
    return std::unexpected(error_from_body(kind_t::generic_failure, "resolving workbench root failed: WorkbenchRootUnresolved"));
  }
  if (create && !wb::fsutil::make_path_all(*root)) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "resolving workbench root failed: MakePathFailed"));
  }
  return *root;
}

/// @brief Resolve the `<plan>` positional, with the oracle's two distinct
/// failure messages and exit codes.
auto resolve_plan(context& ctx, db::connection& conn, std::string_view argument)
    -> std::expected<wb::sync::anchor, domain_error> {
  auto found = wb::sync::resolve_plan_argument(conn, argument);
  if (found) {
    return *found;
  }
  switch (found.error()) {
  case wb::sync::sync_error::invalid_input:
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid plan '{}'", argument)));
  case wb::sync::sync_error::not_found:
    return std::unexpected(error_from_body(kind_t::not_found, std::format("plan not found: {}", argument)));
  default:
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("resolving plan '{}' failed: QueryFailed", argument)));
  }
  static_cast<void>(ctx);
}

/// @brief Parse `--filter-mode`, defaulting to `failures` when absent or empty.
auto parse_filter_mode(const cliapp::parsed_args& args) -> std::expected<wb::terminal::mode, domain_error> {
  auto const raw = flag_string(args, "--filter-mode");
  if (!raw || raw->empty()) {
    return wb::terminal::mode::failures;
  }
  auto const parsed = wb::terminal::mode_from_string(*raw);
  if (!parsed) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, std::format("invalid --filter-mode '{}' (expected 'failures' or 'all')", *raw)));
  }
  return *parsed;
}

/// @brief The Zig error tag for a sync failure, as the oracle interpolates it.
auto sync_error_tag(wb::sync::sync_error err) -> std::string_view {
  switch (err) {
  case wb::sync::sync_error::not_found:
    return "NotFound";
  case wb::sync::sync_error::query_failed:
    return "QueryFailed";
  case wb::sync::sync_error::invalid_input:
    return "InvalidInput";
  case wb::sync::sync_error::io_failed:
    return "RenameFailed";
  }
  return "QueryFailed";
}

/// @brief The shared tail of `pull` / `push` / `sync`: write the payload,
/// then fail on malformed files and then on conflicts, in that order.
///
/// The ORDER is observable — a run with both a malformed file and a
/// conflict exits 1, not 3, because the malformed check comes first.
auto finish_sync_run(context& ctx, const cliapp::parsed_args& args, const wb::sync::anchor& plan, wb::sync::mode run_mode,
                     std::string_view verb, const wb::sync::result& value) -> handler_result {
  if (wants_json(args)) {
    ctx.out() << wb::render_cli::render_sync_result_json(value);
  } else {
    ctx.out() << wb::render_cli::render_sync_result_text(plan.id, plan.slug, run_mode, verb, value, flag_bool(args, "--verbose"));
  }
  if (value.malformed > 0) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, wb::render_cli::error_body_malformed(value.malformed, plan.id)));
  }
  // `status` never fails on conflicts; the other three do.
  if (run_mode != wb::sync::mode::status && value.conflicts > 0) {
    return std::unexpected(error_from_body(kind_t::sync_conflict, wb::render_cli::error_body_conflicts(value.conflicts)));
  }
  return {};
}

/// @brief `pull` / `push` / `sync` share everything but their engine call.
auto run_sync_verb(context& ctx, const cliapp::parsed_args& args, wb::sync::mode run_mode, std::string_view verb,
                   const auto& invoke) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const argument = positional_string(args, "plan").value_or(std::string{});
  auto       plan     = resolve_plan(ctx, **conn, argument);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  auto root = resolve_root(ctx, false);
  if (!root) {
    return std::unexpected(root.error());
  }
  auto value = invoke(**conn, plan->id, *root);
  if (!value) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("workbench {} failed: {}", verb, sync_error_tag(value.error()))));
  }
  return finish_sync_run(ctx, args, *plan, run_mode, verb, *value);
}

} // namespace

auto workbench_pull(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_sync_verb(
      ctx, args, wb::sync::mode::pull, "pull",
      [](db::connection& conn, std::int64_t plan_id, std::string_view root) { return wb::sync::pull(conn, plan_id, root); });
}

auto workbench_sync(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_sync_verb(
      ctx, args, wb::sync::mode::sync, "sync",
      [](db::connection& conn, std::int64_t plan_id, std::string_view root) { return wb::sync::sync_both(conn, plan_id, root); });
}

auto workbench_push(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const filter_mode = parse_filter_mode(args);
  if (!filter_mode) {
    return std::unexpected(filter_mode.error());
  }
  bool const apply_cleanup = flag_bool(args, "--apply-cleanup");
  // The two flags express opposite intents — include every terminal vs.
  // clean every terminal — so the oracle refuses the combination outright
  // rather than picking one.
  if (apply_cleanup && *filter_mode == wb::terminal::mode::all) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, "--apply-cleanup is mutually exclusive with --filter-mode all"));
  }
  return run_sync_verb(ctx, args, wb::sync::mode::push, "push",
                       [&](db::connection& conn, std::int64_t plan_id, std::string_view root) {
                         return wb::sync::push(conn, plan_id, root, *filter_mode, apply_cleanup);
                       });
}

auto workbench_status(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto root = resolve_root(ctx, false);
  if (!root) {
    return std::unexpected(root.error());
  }

  auto const argument = optional_positional(args, "plan");
  if (argument) {
    auto plan = resolve_plan(ctx, **conn, *argument);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto value = wb::sync::status(**conn, plan->id, *root);
    if (!value) {
      return std::unexpected(
          error_from_body(kind_t::generic_failure, std::format("workbench status failed: {}", sync_error_tag(value.error()))));
    }
    return finish_sync_run(ctx, args, *plan, wb::sync::mode::status, "status", *value);
  }

  // No plan argument: fold every feature that HAS a tree into one total.
  auto items = wb::sync::list_active(**conn, *root);
  if (!items) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("workbench status failed: {}", sync_error_tag(items.error()))));
  }
  bool const                    json = wants_json(args);
  wb::render_cli::status_totals totals;
  bool                          any_tree = false;
  for (auto const& item : *items) {
    if (!item.has_fs_tree) {
      continue;
    }
    any_tree   = true;
    auto value = wb::sync::status(**conn, item.plan_id, *root);
    if (!value) {
      return std::unexpected(error_from_body(kind_t::generic_failure, std::format("workbench status failed for plan {}: {}",
                                                                                  item.plan_id, sync_error_tag(value.error()))));
    }
    totals.applied += value->applied;
    totals.pending += value->pending;
    totals.conflicts += value->conflicts;
    totals.malformed += value->malformed;
    for (auto const& file : value->malformed_files) {
      totals.malformed_files.push_back(file);
    }
    if (!json) {
      ctx.out() << wb::render_cli::render_sync_result_text(item.plan_id, item.slug, wb::sync::mode::status, "status", *value,
                                                           flag_bool(args, "--verbose"));
    }
  }
  if (json) {
    // NOTE the totals payload never carries `filtered` / `cleaned` values
    // other than zero: `status` runs never filter. The FIELDS are still
    // emitted, because the oracle emits them.
    ctx.out() << wb::render_cli::render_status_totals_json(totals);
  } else if (!any_tree) {
    ctx.out() << wb::render_cli::render_no_active_features();
  }
  if (totals.malformed > 0) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, wb::render_cli::error_body_malformed(totals.malformed, std::nullopt)));
  }
  return {};
}

auto workbench_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const raw_event = positional_string(args, "event-id").value_or(std::string{});
  auto const event_id  = cliapp::parse_int64_zig(raw_event);
  if (!event_id) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, std::format("event-id must be an integer, got '{}'", raw_event)));
  }
  if (*event_id < 1) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, std::format("invalid event-id '{}' (must be a positive integer)", raw_event)));
  }
  auto const                    raw_prefer = flag_string(args, "--prefer").value_or(std::string{});
  wb::sync::conflict_resolution prefer{};
  if (raw_prefer == "fs") {
    prefer = wb::sync::conflict_resolution::fs;
  } else if (raw_prefer == "db") {
    prefer = wb::sync::conflict_resolution::db;
  } else {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("--prefer must be fs|db, got '{}'", raw_prefer)));
  }
  auto root = resolve_root(ctx, false);
  if (!root) {
    return std::unexpected(root.error());
  }
  auto const settled = wb::sync::resolve_conflict(**conn, *root, *event_id, prefer);
  if (!settled) {
    // The oracle interpolates the raw Zig tag here rather than composing a
    // sentence, so the message reads `workbench resolve failed: NotFound`.
    // `InvalidInput` maps to exit 2 and `NotFound` to exit 1 — probed by
    // resolving an unknown id and then re-resolving a settled one.
    auto const kind = settled.error() == wb::sync::sync_error::invalid_input ? kind_t::invalid_input : kind_t::generic_failure;
    return std::unexpected(error_from_body(kind, std::format("workbench resolve failed: {}", sync_error_tag(settled.error()))));
  }
  ctx.out() << wb::render_cli::render_resolve(*event_id, prefer, wants_json(args));
  return {};
}

auto workbench_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const argument = positional_string(args, "plan").value_or(std::string{});
  auto       plan     = resolve_plan(ctx, **conn, argument);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  auto root = resolve_root(ctx, true);
  if (!root) {
    return std::unexpected(root.error());
  }
  // `--filter-mode` is parsed (so an invalid value still fails at exit 2,
  // as the oracle does) and then unused: `archive` deletes the tree rather
  // than packaging it, so there is no write set to filter.
  auto const filter_mode = parse_filter_mode(args);
  if (!filter_mode) {
    return std::unexpected(filter_mode.error());
  }
  auto const feature_dir = wb::sync::archive(**conn, plan->id, *root);
  if (!feature_dir) {
    return std::unexpected(error_from_body(kind_t::generic_failure,
                                           std::format("workbench archive failed: {}", sync_error_tag(feature_dir.error()))));
  }
  ctx.out() << wb::render_cli::render_archive(plan->id, *feature_dir, wants_json(args));
  return {};
}

auto workbench_restore(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const argument = positional_string(args, "plan").value_or(std::string{});
  auto       plan     = resolve_plan(ctx, **conn, argument);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  auto root = resolve_root(ctx, true);
  if (!root) {
    return std::unexpected(root.error());
  }
  auto const filter_mode = parse_filter_mode(args);
  if (!filter_mode) {
    return std::unexpected(filter_mode.error());
  }
  auto const feature_dir = wb::sync::restore(**conn, plan->id, *root, *filter_mode);
  if (!feature_dir) {
    return std::unexpected(error_from_body(kind_t::generic_failure,
                                           std::format("workbench restore failed: {}", sync_error_tag(feature_dir.error()))));
  }
  ctx.out() << wb::render_cli::render_restore(plan->id, *feature_dir, wants_json(args));
  return {};
}

auto workbench_gc(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const filter_mode = parse_filter_mode(args);
  if (!filter_mode) {
    return std::unexpected(filter_mode.error());
  }
  auto root = resolve_root(ctx, true);
  if (!root) {
    return std::unexpected(root.error());
  }
  bool const            all_scopes = flag_bool(args, "--all-scopes");
  bool const            dry_run    = flag_bool(args, "--dry-run");
  wb::gc::options const opts{
      .dry_run = dry_run, .yes = flag_bool(args, "--yes"), .filter_mode = *filter_mode, .all_scopes = all_scopes};

  std::expected<wb::gc::summary, wb::gc::gc_error> value;
  if (all_scopes) {
    value = wb::gc::run_all_scopes(**conn, *root, opts);
  } else {
    auto const argument = optional_positional(args, "plan");
    if (!argument) {
      return std::unexpected(error_from_body(kind_t::invalid_input, "plan argument required unless --all-scopes is set"));
    }
    auto plan = resolve_plan(ctx, **conn, *argument);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    value = wb::gc::run_for_plan(**conn, plan->id, *root, opts);
  }
  if (!value) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "workbench gc failed: QueryFailed"));
  }

  // THE DRIFT REFUSAL IS THE ONE SANCTIONED DIVERGENCE FROM THE ORACLE IN
  // THIS BUCKET: it prints, and the oracle does not (task 6122).
  //
  // The oracle loses the message. zig/src/cmd/planar/handlers/workbench/
  // gc.zig:42-49 writes the refusal to the BUFFERED `ctx.stderr` and then
  // calls `std.process.exit(1)` DIRECTLY. Every other failure path in that
  // binary goes through `exit.die`, which calls `runtime.shutdown()` — the
  // flush — before exiting (zig/src/cmd/planar/exit.zig:73-79). Skipping
  // shutdown discards the buffer, so the operator gets a bare exit 1 with no
  // reason. Verified against the oracle with stderr on a terminal rather than
  // redirected, so it is not a capture artifact.
  //
  // This was previously reproduced under D2, on the reasoning that emitting
  // would break the differential harness. It would not: NEITHER harness ever
  // reaches this path. The Zig integration suite's `workbench_gc_test.zig`
  // has two cases and drifts no file, and the differential sweep in
  // src/cmd/planar/parity.t.cpp runs `workbench gc` only as `--dry-run`,
  // `--all-scopes --json`, and with no plan. So the whole cost of the D2
  // reproduction was an operator-facing refusal that says nothing, for the
  // remaining life of zig/, bought for no parity signal at all.
  //
  // If a drift step is ever ADDED to either harness, it must expect this
  // divergence rather than treat it as a regression.
  //
  // The text comes from `render_cli::render_gc_drift_refusal`, which renders
  // exactly what the Zig source intends to print and is pinned in
  // render_cli.t.cpp.
  if (value->drifted_skipped > 0 && !opts.yes) {
    return std::unexpected(error_from_rendered(kind_t::generic_failure, wb::render_cli::render_gc_drift_refusal(*value)));
  }

  ctx.out() << (wants_json(args) ? wb::render_cli::render_gc_json(*value, dry_run, *filter_mode)
                                 : wb::render_cli::render_gc_text(*value, dry_run, *filter_mode));
  return {};
}

auto workbench_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto root = resolve_root(ctx, true);
  if (!root) {
    return std::unexpected(root.error());
  }
  auto items = wb::sync::list_active(**conn, *root);
  if (!items) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("listing features failed: {}", sync_error_tag(items.error()))));
  }
  ctx.out() << (wants_json(args) ? wb::render_cli::render_list_json(*items) : wb::render_cli::render_list_text(*items));
  return {};
}

auto workbench_lint(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const plan_argument = optional_positional(args, "plan");
  auto const path_flag     = flag_string(args, "--path");
  bool const all           = flag_bool(args, "--all");

  // EXACTLY one target. Zero selectors and two selectors are the same
  // failure and the same message; both probed.
  int const selected =
      static_cast<int>(plan_argument.has_value()) + static_cast<int>(all) + static_cast<int>(path_flag.has_value());
  if (selected != 1) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, "choose exactly one lint target: <plan>, --all, or --path <file-or-directory>"));
  }

  std::string target;
  if (path_flag) {
    target = *path_flag;
  } else {
    auto root = resolve_root(ctx, false);
    if (!root) {
      return std::unexpected(root.error());
    }
    if (all) {
      target = *root;
    } else {
      auto plan = resolve_plan(ctx, **conn, *plan_argument);
      if (!plan) {
        return std::unexpected(plan.error());
      }
      target = wb::sync::feature_dir_for(*root, *plan);
    }
  }

  auto value = wb::lint::run(**conn, target);
  if (!value) {
    switch (value.error()) {
    case wb::lint::lint_error::not_found:
      return std::unexpected(error_from_body(kind_t::generic_failure, std::format("lint target not found: {}", target)));
    case wb::lint::lint_error::invalid_input:
      return std::unexpected(
          error_from_body(kind_t::invalid_input, std::format("lint target must be a Markdown file or directory: {}", target)));
    default:
      return std::unexpected(
          error_from_body(kind_t::generic_failure, std::format("workbench lint failed for {}: QueryFailed", target)));
    }
  }

  ctx.out() << (wants_json(args) ? wb::render_cli::render_lint_json(value->issues) : wb::render_cli::render_lint_text(*value));

  // WARNINGS ALONE still fail the verb. Oracle-probed.
  if (value->errors > 0 || value->warnings > 0) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, wb::render_cli::error_body_lint_issues(value->errors, value->warnings)));
  }
  return {};
}

auto workbench_extract_questions(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const argument = positional_string(args, "plan").value_or(std::string{});
  auto       plan     = resolve_plan(ctx, **conn, argument);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  // A plain resolve, NOT the ensuring one: a read-only leaf must not leave a
  // workbench root behind on a machine that has none. Mirrors the oracle's
  // bare `resolveRoot` call here versus `resolveAndEnsureWorkbenchRoot` in
  // `list` / `gc` / `archive` / `restore`.
  auto root = resolve_root(ctx, false);
  if (!root) {
    return std::unexpected(root.error());
  }
  auto const feature_dir = wb::sync::feature_dir_for(*root, *plan);

  // An ABSENT tree is a HINT at exit 0, not a failure — this leaf reports on
  // what `push` wrote and has nothing to say before the first push.
  if (!wb::fsutil::path_exists(feature_dir)) {
    ctx.out() << std::format("workbench tree not found for plan {}; run 'workbench push {}' first\n", plan->id, argument);
    return {};
  }

  std::vector<wb::questions::file_questions> results;
  for (auto const& name : wb::questions::collect_top_level_specs(feature_dir)) {
    auto const contents = wb::fsutil::read_file(std::filesystem::path{feature_dir} / name);
    if (!contents) {
      // The oracle interpolates a Zig error tag here. Reaching this arm at
      // all needs the file to vanish between the directory listing and the
      // read, so neither binary can produce it deterministically and no
      // test pins the tag.
      ctx.err() << std::format("warning: reading {} failed: ReadFailed\n", name);
      continue;
    }
    auto const parsed = wb::parse::parse(*contents);
    if (!parsed) {
      // A MALFORMED FILE IS SWALLOWED, and that is reproduced deliberately
      // (D2), not overlooked. The same file `workbench push` rejects as
      // `MissingRequiredField` at exit 1 is skipped silently here, so a
      // spec with broken front matter reports zero questions rather than an
      // error. Filed as planar task 6304; changing it is a behaviour
      // change that belongs in that task, not smuggled into a port.
      // TODO(plan:996, task:6304): decide whether to surface these.
      continue;
    }
    results.push_back({.artifact_id = parsed->frontmatter.entity_id,
                       .file        = name,
                       .questions   = wb::questions::extract_questions(parsed->body)});
  }

  if (wants_json(args)) {
    ctx.out() << wb::questions::render_json(results) << "\n";
  } else {
    ctx.out() << wb::questions::render_text(results);
  }
  return {};
}

auto workbench_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const argument = positional_string(args, "plan").value_or(std::string{});
  auto       plan     = resolve_plan(ctx, **conn, argument);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  // The oracle resolves the root AFTER the push, because its `push` reaches
  // the environment itself. Here every engine entry point takes the root as
  // a parameter (see engine/workbench/CMakeLists.txt § FILESYSTEM SAFETY),
  // so it is resolved once up front and used for both halves. `create` is
  // true to match the oracle's `resolveAndEnsureWorkbenchRoot`; the push
  // would create the feature directory underneath it regardless, so the
  // reordering is not observable.
  auto root = resolve_root(ctx, true);
  if (!root) {
    return std::unexpected(root.error());
  }

  // Always the default filter and never a cleanup: `workbench edit` exposes
  // neither `--filter-mode` nor `--apply-cleanup`.
  auto pushed = wb::sync::push(**conn, plan->id, *root, wb::terminal::mode::failures, false);
  if (!pushed) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("workbench push failed: {}", sync_error_tag(pushed.error()))));
  }
  if (wants_json(args)) {
    ctx.out() << wb::render_cli::render_sync_result_json(*pushed);
  } else {
    // `verbose` is hard-false: the editor-first flow prints a summary line,
    // never the per-entry list.
    ctx.out() << wb::render_cli::render_sync_result_text(plan->id, plan->slug, wb::sync::mode::push, "push", *pushed, false);
  }
  if (pushed->conflicts > 0) {
    return std::unexpected(error_from_body(kind_t::sync_conflict, wb::render_cli::error_body_conflicts(pushed->conflicts)));
  }

  // THE EDITOR IS GIVEN THE FEATURE DIRECTORY, not a temp file. That is the
  // difference between this leaf and the drafting quartet's `edit` arms,
  // and it is what makes the round trip a `pull` rather than a single-file
  // write-back. Oracle-probed: the stub's `$1` is the feature directory.
  auto const override_flag = flag_string(args, "--editor");
  auto const editor_cmd    = resolve_editor(
      ctx.env(), override_flag && !override_flag->empty() ? std::optional<std::string>{*override_flag} : std::nullopt);
  std::array<std::string, 2> const argv{editor_cmd, wb::sync::feature_dir_for(*root, *plan)};
  auto const                       status = spawn_inherit(ctx.env(), argv);
  if (!status) {
    // Unresolvable command or a failed fork. The oracle's `std.process.spawn`
    // reports this as `error.FileNotFound` BEFORE creating a child, which is
    // why it is distinguishable from a real non-zero exit at all.
    return std::unexpected(error_from_body(kind_t::generic_failure, "running editor failed: FileNotFound"));
  }
  if (*status != 0) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("editor \"{}\" exited with status {}", editor_cmd, *status)));
  }

  auto pulled = wb::sync::pull(**conn, plan->id, *root);
  if (!pulled) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, std::format("workbench pull failed: {}", sync_error_tag(pulled.error()))));
  }
  if (wants_json(args)) {
    ctx.out() << wb::render_cli::render_sync_result_json(*pulled);
  } else {
    ctx.out() << wb::render_cli::render_sync_result_text(plan->id, plan->slug, wb::sync::mode::pull, "pull", *pulled, false);
  }
  if (pulled->conflicts > 0) {
    return std::unexpected(error_from_body(kind_t::sync_conflict, wb::render_cli::error_body_conflicts(pulled->conflicts)));
  }
  return {};
}

} // namespace planar::cmd::handlers
