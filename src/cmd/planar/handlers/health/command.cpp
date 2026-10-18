/// @file src/cmd/planar/handlers/health/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.health`.
/// See health.cppm for the exit-code and scope conventions.

module planar.cmd.planar.handlers.health;

import std;
import planar.buildqueue.path;
import planar.buildqueue.reader;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.config;
import planar.engine.health;
import planar.engine.identity;
import planar.engine.runtime.completion;
import planar.engine.runtime.conformance;
import planar.installed_surface;
import planar.cmd.internal.config_path;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import cli11;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.health.hygiene;
import planar.cmd.planar.handlers.health.conformance;

namespace planar::cmd::handlers {

namespace he   = engine::health;
namespace is_  = installed_surface;
namespace cf   = engine::runtime::conformance;
namespace comp = engine::runtime::completion;
namespace cfg  = engine::config;
namespace bq   = buildqueue;

namespace {

/// @brief Emit an `std::optional<std::string>` field the way `render_json`
/// does throughout this file: the escaped string, or the bare JSON literal
/// `null` when unset. `report`'s stringification uses std.json's DEFAULT
/// options, so nulls are always emitted, never omitted.
auto json_opt_string(const std::optional<std::string>& value) -> std::string {
  return value.has_value() ? json_text::json_string(*value) : "null";
}

/// @brief Render `report` as the oracle's `--json` payload for `planar
/// health`. Key order follows the engine struct, which follows the
/// oracle's.
/// @param report The report.
/// @return The complete stdout payload including its trailing newline.
auto render_json(const he::report& report) -> std::string {
  std::string out = std::format(
      "{{\"db_path\":{},\"db_ok\":{},\"schema_version\":{},\"schema_target\":{},\"schema_current\":{},\"migration_count\":{},"
      "\"integrity_ok\":{},\"inflight_tasks\":{},\"resumable_tasks\":{},\"not_resumable_tasks\":{},\"pending_handoffs\":{},"
      "\"stale_handoffs\":{},\"projection_freshness\":{{\"state\":{},\"manifest_status\":{},\"managed\":{},\"fresh\":{},"
      "\"stale\":{},\"missing\":{},\"unmanaged\":{},\"unselected_vendors\":{},\"evidence\":{},\"repair_command\":{}}},"
      "\"overall\":{}}}\n",
      json_text::json_string(report.db_path), report.db_ok ? "true" : "false", report.schema_version, report.schema_target,
      report.schema_current ? "true" : "false", report.migration_count, report.integrity_ok ? "true" : "false",
      report.inflight_tasks, report.resumable_tasks, report.not_resumable_tasks, report.pending_handoffs, report.stale_handoffs,
      json_text::json_string(report.projection_freshness.state),
      json_text::json_string(std::string(he::manifest_state_name(report.projection_freshness.manifest_status))),
      report.projection_freshness.managed, report.projection_freshness.fresh, report.projection_freshness.stale,
      report.projection_freshness.missing, report.projection_freshness.unmanaged, report.projection_freshness.unselected_vendors,
      json_opt_string(report.projection_freshness.evidence), json_opt_string(report.projection_freshness.repair_command),
      json_text::json_string(report.overall));
  return out;
}

/// @brief Resolve `$PLANAR_HOME` / `$HOME` / `$CODEX_HOME` exactly as the
/// oracle's `skills/common.zig::resolveHomes` does: `HOME` is required,
/// `PLANAR_HOME` defaults to `<HOME>/.planar`, `CODEX_HOME` defaults to
/// `<HOME>/.codex`.
/// @param ctx The process context, consulted only through `ctx.env()`.
/// @return The three homes, or the `HomeNotSet` refusal.
auto resolve_homes(context& ctx) -> std::expected<is_::options, domain_error> {
  auto const home = ctx.env()("HOME");
  if (!home.has_value() || home->empty()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "health check failed: resolving install homes: HomeNotSet"));
  }
  is_::options opts;
  opts.home            = *home;
  opts.planar_home     = ctx.env()("PLANAR_HOME").value_or((std::filesystem::path(*home) / ".planar").string());
  auto const codex_env = ctx.env()("CODEX_HOME");
  opts.codex_home      = codex_env.value_or((std::filesystem::path(*home) / ".codex").string());
  opts.codex_home_set  = codex_env.has_value() && !codex_env->empty();
  return opts;
}

/// @brief Render the report as the oracle's `--json` payload.
///
/// Key order follows the engine struct, which follows the oracle's. Note
/// that `parent_plan_id` IS emitted as `null` when unset: this verb
/// stringifies with std.json's DEFAULT options, unlike `audit session`,
/// which passes `emit_null_optional_fields = false` and drops its nulls.
/// Two verbs, two conventions, both captured from live runs.
/// @param report The report.
/// @return The complete stdout payload including its trailing newline.
auto render_json(const he::hygiene_report& report) -> std::string {
  std::string out = std::format("{{\"thresholds\":{{\"stale_doing_days\":{},\"stale_open_days\":{}}},\"stale_draft_plans\":[",
                                report.thresholds.stale_doing_days, report.thresholds.stale_open_days);
  for (std::size_t i = 0; i < report.stale_draft_plans.size(); ++i) {
    auto const& row = report.stale_draft_plans[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"parent_plan_id\":", row.id);
    out += row.parent_plan_id.has_value() ? std::to_string(*row.parent_plan_id) : "null";
    out += ",\"title\":";
    out += json_text::json_string(row.title);
    out += ",\"reason\":";
    out += json_text::json_string(row.reason);
    out += std::format(",\"task_counts\":{{\"todo\":{},\"doing\":{},\"blocked\":{},\"done\":{},\"cancelled\":{}}}",
                       row.counts.todo, row.counts.doing, row.counts.blocked, row.counts.done, row.counts.cancelled);
    out += ",\"suggestion\":";
    out += json_text::json_string(row.suggestion);
    out += "}";
  }

  out += "],\"stale_doing_tasks\":[";
  for (std::size_t i = 0; i < report.stale_doing_tasks.size(); ++i) {
    auto const& row = report.stale_doing_tasks[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"plan_id\":{},\"scope\":", row.id, row.plan_id);
    out += json_text::json_string(row.scope);
    out += ",\"title\":";
    out += json_text::json_string(row.title);
    out += std::format(",\"age_days\":{},\"suggestion\":", row.age_days);
    out += json_text::json_string(row.suggestion);
    out += "}";
  }

  out += "],\"stale_open_questions\":[";
  for (std::size_t i = 0; i < report.stale_open_questions.size(); ++i) {
    auto const& row = report.stale_open_questions[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"title\":", row.id);
    out += json_text::json_string(row.title);
    out += std::format(",\"age_days\":{},\"suggestion\":", row.age_days);
    out += json_text::json_string(row.suggestion);
    out += "}";
  }

  out += "]}\n";
  return out;
}

/// @brief Map an engine failure to its oracle message. ALL THREE are exit
/// 1 — none of them is `invalid_input`, even the negative-threshold one,
/// because the oracle dies through `error.InvalidThreshold`, which has no
/// arm in its `codeFor` table.
auto map_hygiene_error(he::hygiene_error err) -> domain_error {
  switch (err) {
  case he::hygiene_error::invalid_threshold:
    return error_from_body(domain_error_kind::generic_failure, "stale thresholds must be non-negative");
  case he::hygiene_error::unsupported_scope:
    return error_from_body(domain_error_kind::generic_failure, "--scope must name one association");
  case he::hygiene_error::slug_not_found:
    return error_from_body(domain_error_kind::generic_failure, "scope slug not found");
  case he::hygiene_error::query_failed:
    return error_from_body(domain_error_kind::generic_failure, "health hygiene failed: QueryFailed");
  }
  return error_from_body(domain_error_kind::generic_failure, "health hygiene failed: QueryFailed");
}

/// @brief The scope slug a plan is stored at, as `engine::identity` labels it (unset for global).
/// @param conn An open, migrated connection.
/// @param plan_id The plan.
/// @return The label, or the refusal. A missing plan is `not_found`.
auto plan_scope_label(db::connection& conn, std::int64_t plan_id) -> std::expected<std::optional<std::string>, domain_error> {
  auto stmt = conn.prepare("select scope_kind, scope_id from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "health conformance: QueryFailed"));
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "health conformance: QueryFailed"));
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", plan_id)));
  }
  auto const                  text = stmt->column_text(0);
  auto const                  kind = text == "association" ? engine::identity::scope_kind::association
                                     : text == "repo"      ? engine::identity::scope_kind::repo
                                                           : engine::identity::scope_kind::global;
  std::optional<std::int64_t> id;
  if (!stmt->is_null(1)) {
    id = stmt->column_int64(1);
  }
  auto label = engine::identity::slug_from_ref(conn, kind, id);
  if (!label) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "health conformance: reading the plan's scope failed"));
  }
  return *label;
}

/// @brief The policy resolver `conformance::check` is given: the effective policy of a plan's scope from the located
/// `config.toml`, the same resolution `planar-agent claim` and `complete` apply. No environment variable participates.
/// @param doc The parsed config file.
/// @return A resolver; its failure text says why the policy cannot be applied.
auto conformance_policy_resolver(cfg::toml_map doc) -> cf::policy_resolver {
  return [doc = std::move(doc)](db::connection& conn, std::int64_t plan_id) -> std::expected<cf::policy_view, std::string> {
    auto const association = cfg::policy_association_for_plan(conn, plan_id);
    if (!association) {
      return std::unexpected(std::format("the policy scope of plan {} could not be read", plan_id));
    }
    auto const slug = association->slug;
    auto const resolved =
        cfg::effective_policy(doc, slug.has_value() ? std::optional<std::string_view>{std::string_view{*slug}} : std::nullopt);
    if (!resolved) {
      std::string problem;
      for (auto const& finding : resolved.error()) {
        problem += problem.empty() ? "" : "; ";
        problem += std::format("{}: {}", finding.key, finding.message);
      }
      return std::unexpected(std::format("the policy in config.toml cannot be applied: {}\n"
                                         "  fix the file, then check it with: planar config validate",
                                         problem));
    }
    auto const& values = resolved->values;
    return cf::policy_view{.review_required = values.review_required,
                           .task_gate       = values.task_gate,
                           .require_model   = values.require_model,
                           .use_queue       = values.use_queue,
                           .digest          = cfg::policy_digest(values)};
  };
}

/// @brief The runs tagged with a claim's token, read from the build queue database (read-only).
/// @param path The database path, or the resolver's failure.
/// @return A lookup for rule 5, equal in meaning to the one `planar-agent complete` uses.
auto history_gate_lookup(std::expected<std::filesystem::path, bq::error> path) -> comp::gate_run_lookup {
  return [path = std::move(path)](std::string_view tag) -> std::expected<std::vector<comp::gate_run>, std::string> {
    if (!path) {
      return std::unexpected(bq::error_message(path.error()));
    }
    auto const read = bq::read_tagged_runs(*path, tag);
    if (!read) {
      return std::unexpected(read.error().reason);
    }
    std::vector<comp::gate_run> runs;
    runs.reserve(read->runs.size());
    for (auto const& run : read->runs) {
      runs.push_back(comp::gate_run{.argv   = run.argv,
                                    .cwd    = run.cwd,
                                    .passed = run.outcome == "exited" && run.exit_code.has_value() && *run.exit_code == 0});
    }
    return runs;
  };
}

/// @brief The build queue database's header for rule 5's evidence horizon (read-only).
/// @param path The database path, or the resolver's failure.
/// @return A reader that returns the header, or the reason the database cannot be read.
auto queue_header_source(std::expected<std::filesystem::path, bq::error> path) -> cf::queue_header_reader {
  return [path = std::move(path)]() -> std::expected<cf::queue_header, std::string> {
    if (!path) {
      return std::unexpected(bq::error_message(path.error()));
    }
    auto const facts = bq::read_queue_facts(*path);
    if (!facts) {
      return std::unexpected(facts.error().reason);
    }
    return cf::queue_header{.path                = path->string(),
                            .present             = facts->state == bq::reader_state::present,
                            .created_at          = facts->created_at,
                            .tagged_history_days = facts->tagged_history_days};
  };
}

} // namespace

auto health(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto base_report = he::check(**conn, ctx.db_path().string());
  if (!base_report) {
    auto const message = base_report.error() == he::check_error::schema_table_missing
                             ? std::format("health check failed: {}", "SchemaTableMissing")
                             : std::format("health check failed: {}", "QueryFailed");
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, message));
  }

  auto homes = resolve_homes(ctx);
  if (!homes) {
    return std::unexpected(homes.error());
  }

  auto installed = is_::status(*homes);
  if (!installed) {
    auto const name = installed.error() == is_::status_error::invalid_input ? "InvalidInput" : "InvalidVendor";
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("health check failed: installed projection status: {}", name)));
  }

  auto const report = he::with_projection_freshness(std::move(*base_report), *installed);

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_json(report) : he::render_text(report));

  // Mirror the oracle's exit-1-on-DEGRADED contract (Cluster
  // C-health-content-loss, plan 351 Q235): the report is already written
  // above regardless of outcome, and this is purely how `degraded` reaches
  // dispatch's exit code without printing anything further — see
  // health.cppm's header.
  if (report.overall == "degraded") {
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, ""));
  }
  return {};
}

auto health_hygiene(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  he::hygiene_options options;
  options.scope = cliapp::flag_string(args, "--scope");
  // The defaults live in the surface declaration too, but they are
  // repeated here so the handler is correct when the flag machinery hands
  // back nothing — the oracle's own defaults are 7 and 30.
  options.stale_doing_days = cliapp::flag_int(args, "--stale-doing").value_or(7);
  options.stale_open_days  = cliapp::flag_int(args, "--stale-open").value_or(30);

  auto report = he::hygiene(**conn, options);
  if (!report) {
    return std::unexpected(map_hygiene_error(report.error()));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_json(*report) : he::render_hygiene_text(*report));
  return {};
}

auto health_conformance(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const plan_id = entity_id_arg(args, "plan-id", "plan");
  if (!plan_id) {
    return std::unexpected(plan_id.error());
  }
  auto const floor = cliapp::flag_int(args, "--floor");
  if (floor.has_value() && *floor < 1) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--floor must be a positive claim id"));
  }
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const label = plan_scope_label(**conn, *plan_id);
  if (!label) {
    return std::unexpected(label.error());
  }
  if (auto const scope = cliapp::flag_string(args, "--scope"); scope.has_value()) {
    // A read filter, not a write guard: the write guard lets any scope reach a global entity, which would make
    // `--scope` filter nothing. A global plan is in scope `global` and nowhere else; any other plan is in the
    // scope it is stored at, or in an association that its repository belongs to.
    auto const entity_view = label->has_value() ? std::optional<std::string_view>{**label} : std::nullopt;
    bool const in_scope =
        label->has_value() ? guard_with_membership(**conn, entity_view, std::string_view{*scope}) : *scope == "global";
    if (!in_scope) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure,
                          std::format("plan {} is in scope '{}', not '{}'", *plan_id, label->value_or("global"), *scope)));
    }
  }

  auto const path = internal::resolve_config_path(ctx.env());
  auto       doc  = path.has_value() ? cfg::load_toml_file(*path) : std::expected<cfg::toml_map, std::string>{cfg::toml_map{}};
  if (!doc) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("health conformance: the policy in config.toml cannot be applied: {}\n"
                                                       "  fix the file, then check it with: planar config validate",
                                                       doc.error())));
  }

  auto const queue_path = bq::resolve_queue_db_path(ctx.env());
  auto const graded =
      cf::check(**conn, cf::options{.plan_id = *plan_id, .floor_claim_id = floor, .now = std::nullopt},
                conformance_policy_resolver(std::move(*doc)), history_gate_lookup(queue_path), queue_header_source(queue_path));
  if (!graded) {
    switch (graded.error().kind) {
    case cf::error_kind::plan_not_found:
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no plan with id {}", *plan_id)));
    case cf::error_kind::policy_unavailable:
    case cf::error_kind::queue_unreadable:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("health conformance: {}", graded.error().detail)));
    case cf::error_kind::query_failed:
      break;
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "health conformance: QueryFailed"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? cf::render_json(*graded) : cf::render_text(*graded));
  if (!graded->conforms()) {
    // The report is already written; the exit code is how a script learns the trail broke a rule.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("plan {} does not conform: {} violation{}", *plan_id,
                                                       graded->violations.size(), graded->violations.size() == 1 ? "" : "s")));
  }
  return {};
}

/// @brief Declare the `health` group and its `hygiene` child.
///
/// THE LAST ANCESTOR-FLAGS SITE, and the reason this one is written
/// as a single contiguous list with `--json` declared LAST on `hygiene`.
///
/// `health` was the only generated parent in the tree that both
/// carried a flag and had a child, so `apply_surface`'s ANCESTOR FLAGS
/// block fired here and nowhere else: it declared `hygiene`'s own three
/// flags first and APPENDED the inherited `--json` after them. The
/// schema catalog cannot see that order (`render_flags` emits inherited
/// flags first regardless), but `hygiene --help` can, and
/// `scripts/surface-snapshot.sh` hashes every leaf's help page. So the
/// `--json` call below must stay where it is — moving it above
/// `--scope` is a real, gate-caught change to the shipped help page.
///
/// This is the mirror image of `declare_handoff`, where the hand half
/// declared inherited flags FIRST and the fold preserved that. Both
/// orders ship somewhere; neither is a convention.
auto declare_health(CLI::App& root) -> void {
  CLI::App* health = root.add_subcommand(
      "health", "Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, pending "
                "handoff staleness,\n  and manifest-owned installed projection freshness. This command is\n  read-only; recovery "
                "commands are reported but never run.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks "
                "not resumable, stale handoffs, stale\n       or missing managed projections, integrity errors, etc.)");
  health->require_subcommand(0);
  add_json(*health, "Emit machine-readable JSON instead of text");

  health_cli::attach_hygiene(health);
  health_cli::attach_conformance(health);
}

} // namespace planar::cmd::handlers
