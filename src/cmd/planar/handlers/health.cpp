/// @file health.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.health`.
/// See health.cppm for the exit-code and scope conventions.

module planar.cmd.planar.handlers.health;

import std;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.health;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace he = engine::health;

namespace {

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

auto health_hygiene(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
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

} // namespace planar::cmd::handlers
