/// @file src/cmd/planar/handlers/health/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.health`.
/// See health.cppm for the exit-code and scope conventions.

module planar.cmd.planar.handlers.health;

import std;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.health;
import planar.installed_surface;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import cli11;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.health.hygiene;

namespace planar::cmd::handlers {

namespace he  = engine::health;
namespace is_ = installed_surface;

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
  opts.home        = *home;
  opts.planar_home = ctx.env()("PLANAR_HOME").value_or((std::filesystem::path(*home) / ".planar").string());
  opts.codex_home  = ctx.env()("CODEX_HOME").value_or((std::filesystem::path(*home) / ".codex").string());
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
  add_json(*health, k_undocumented);

  health_cli::attach_hygiene(health);
}

} // namespace planar::cmd::handlers
